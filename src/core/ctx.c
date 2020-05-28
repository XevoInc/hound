/**
 * @file      ctx.c
 * @brief     Hound context tracking subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _GNU_SOURCE
#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/error.h>
#include <hound-private/log.h>
#include <hound-private/util.h>
#include <pthread.h>
#include <stdbool.h>
#include <xlib/xhash.h>
#include <xlib/xvec.h>

/*
 * The larger the buffer, the more efficient we can be (fewer dequeue
 * operations), as long as we don't overflow the stack.
 */
#define DEQUEUE_BUF_SIZE (4096 / sizeof(struct hound_record_info *))

/* driver --> list of data needed from that driver */
XHASH_MAP_INIT_PTR(DRIVER_DATA_MAP, struct driver *, struct hound_data_rq_list) /* NOLINT */
/*
 * TODO: We shouldn't have to add NOLINT here; it should be entirely contained
 * in the xhash header. Strangely, that does not seem to be working.
 */

struct hound_ctx {
    pthread_rwlock_t rwlock;

    bool active;
    size_t readers;
    hound_cb cb;
    void *cb_ctx;
    struct queue *queue;
    xhash_t(DRIVER_DATA_MAP) *periodic_data_map;
    xhash_t(DRIVER_DATA_MAP) *on_demand_data_map;
};

static
void destroy_driver_data_map(xhash_t(DRIVER_DATA_MAP) *map)
{
    xhiter_t iter;

    xh_iter(map, iter,
        destroy_rq_list(&xh_val(map, iter));
    );
    xh_destroy(DRIVER_DATA_MAP, map);
}

static
hound_err validate_rq(const struct hound_rq *rq)
{
    struct hound_data_rq *data_rq;
    struct driver *drv;
    hound_err err;
    size_t i;
    size_t j;
    const struct hound_data_rq_list *list;

    /* Is the request sane? */
    if (rq->queue_len == 0) {
        return HOUND_EMPTY_QUEUE;
    }

    list = &rq->rq_list;
    if (list->len == 0) {
        return HOUND_NO_DATA_REQUESTED;
    }

    if (list->len > HOUND_MAX_DATA_REQ) {
        return HOUND_TOO_MUCH_DATA_REQUESTED;
    }

    NULL_CHECK(list->data);

    if (rq->cb == NULL) {
        return HOUND_MISSING_CALLBACK;
    }

    /* Are the data IDs all valid? */
    for (i = 0; i < list->len; ++i) {
        data_rq = &list->data[i];

        err = driver_get(data_rq->id, &drv);
        if (err != HOUND_OK) {
            return err;
        }

        for (j = 0; j < i; ++j) {
            if (data_rq->id == list->data[j].id) {
                if (data_rq->period_ns == list->data[j].period_ns ||
                    driver_is_push_mode(drv)) {
                    /*
                     * Push-mode drivers can push data at only one rate, so this is
                     * an error. Pull-mode drivers can handle the same data at
                     * multiple frequencies without issue, but the exact same ID
                     * and frequency should still not be requested.
                     */
                    return HOUND_DUPLICATE_DATA_REQUESTED;
                }
            }
        }

        if (!driver_period_supported(drv, data_rq->id, data_rq->period_ns)) {
            return HOUND_PERIOD_UNSUPPORTED;
        }
    }

    return HOUND_OK;
}

static
hound_err make_driver_data_maps(
    const struct hound_data_rq_list *list,
    xhash_t(DRIVER_DATA_MAP) **out_periodic_map,
    xhash_t(DRIVER_DATA_MAP) **out_on_demand_map)
{
    struct hound_data_rq *data_rq;
    struct driver *drv;
    struct driver *drv_iter;
    hound_err err;
    size_t i;
    size_t index;
    xhiter_t iter;
    size_t j;
    xhash_t(DRIVER_DATA_MAP) *map;
    size_t matches;
    xhash_t(DRIVER_DATA_MAP) *periodic_map;
    xhash_t(DRIVER_DATA_MAP) *on_demand_map;
    int ret;
    struct hound_data_rq_list *rq_list;

    periodic_map = xh_init(DRIVER_DATA_MAP);
    if (periodic_map == NULL) {
        err = HOUND_OOM;
        goto error_periodic_map;
    }

    on_demand_map = xh_init(DRIVER_DATA_MAP);
    if (on_demand_map == NULL) {
        err = HOUND_OOM;
        goto error_on_demand_map;
    }

    for (i = 0; i < list->len; ++i) {
        data_rq = &list->data[i];

        err = driver_get(data_rq->id, &drv);
        if (err != HOUND_OK) {
            goto error_loop;
        }

        /*
         * We want to process all the entries for a given driver all at once, so
         * if we've already seen this driver, just continue on.
         */
        if (xh_found(DRIVER_DATA_MAP, periodic_map, drv) ||
            xh_found(DRIVER_DATA_MAP, on_demand_map, drv)) {
            continue;
        }

        /*
         * This is the first time we've seen this driver. Add an entry to the
         * map and populate it with all the data corresponding to this driver.
         */
        if (data_rq->period_ns != 0) {
            map = periodic_map;
        }
        else {
            map = on_demand_map;
        }
        iter = xh_put(DRIVER_DATA_MAP, map, drv, &ret);
        if (ret == -1) {
            err = HOUND_OOM;
            goto error_loop;
        }
        rq_list = &xh_val(map, iter);
        rq_list->data = NULL;

        /* Look ahead to count how many requests match this driver. */
        matches = 1;
        for (j = i+1; j < list->len; ++j) {
            err = driver_get(list->data[j].id, &drv_iter);
            if (err != HOUND_OK) {
                goto error_loop;
            }
            if (drv_iter == drv) {
                ++matches;
            }
        }
        rq_list->len = matches;
        rq_list->data = malloc(
            rq_list->len * sizeof(*rq_list->data));
        if (rq_list->data == NULL) {
            err = HOUND_OOM;
            goto error_loop;
        }

        /* Now populate the request list. */
        index = 0;
        for (j = i; j < list->len; ++j) {
            err = driver_get(list->data[j].id, &drv_iter);
            if (err != HOUND_OK) {
                goto error_loop;
            }
            if (drv_iter != drv) {
                continue;
            }
            rq_list->data[index].id = list->data[j].id;
            rq_list->data[index].period_ns = list->data[j].period_ns;
            ++index;
        }
    }
    xh_trim(DRIVER_DATA_MAP, periodic_map);
    xh_trim(DRIVER_DATA_MAP, on_demand_map);

    *out_periodic_map = periodic_map;
    *out_on_demand_map = on_demand_map;

    return HOUND_OK;

error_loop:
    destroy_driver_data_map(on_demand_map);
error_on_demand_map:
    destroy_driver_data_map(periodic_map);
error_periodic_map:
    return err;
}

hound_err ctx_alloc(const struct hound_rq *rq, struct hound_ctx **ctx_out)
{
    struct hound_ctx *ctx;
    hound_err err;

    NULL_CHECK(ctx_out);
    NULL_CHECK(rq);

    err = validate_rq(rq);
    if (err != HOUND_OK) {
        goto out;
    }

    /* Request is fine. Let's make a new context. */
    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    err = pthread_rwlock_init(&ctx->rwlock, NULL);
    if (err != 0) {
        goto error_pthread_init;
    }

    ctx->active = false;
    ctx->readers = 0;
    ctx->cb = rq->cb;
    ctx->cb_ctx = rq->cb_ctx;

    err = queue_alloc(&ctx->queue, rq->queue_len);
    if (err != HOUND_OK) {
        goto error_queue_alloc;
    }

    /* Populate our context. */
    err = make_driver_data_maps(
        &rq->rq_list,
        &ctx->periodic_data_map,
        &ctx->on_demand_data_map);
    if (err != HOUND_OK) {
        goto error_make_data_maps;
    }

    *ctx_out = ctx;
    err = HOUND_OK;
    goto out;

error_make_data_maps:
    queue_destroy(ctx->queue);
error_queue_alloc:
    pthread_rwlock_destroy(&ctx->rwlock);
error_pthread_init:
    free(ctx);
out:
    return err;
}

static
hound_err ref_driver_map(
    struct hound_ctx *ctx,
    xhash_t(DRIVER_DATA_MAP) *map,
    bool modify)
{
    struct driver *drv;
    const struct hound_data_rq_list *rq_list;
    hound_err ref_err;
    xhiter_t ref_iter;
    hound_err unref_err;
    xhiter_t unref_iter;

    xh_iter(map, ref_iter,
        drv = xh_key(map, ref_iter);
        rq_list = &xh_val(map, ref_iter);
        ref_err = driver_ref(drv, ctx->queue, rq_list, modify);
        if (ref_err != HOUND_OK) {
            goto error;
        }
    );

    ref_err = HOUND_OK;
    goto out;

error:
    /* Unref any drivers we reffed. */
    xh_iter(map, unref_iter,
        /* We have hit the the driver that failed to ref, so stop here. */
        if (unref_iter == ref_iter) {
            break;
        }

        drv = xh_key(map, unref_iter);
        rq_list = &xh_val(map, unref_iter);
        unref_err = driver_unref(drv, ctx->queue, rq_list, modify);
        if (unref_err != HOUND_OK) {
            hound_log_err(
                unref_err,
                "ctx %p: failed to unref driver %p", (void *) ctx, (void *) drv);
        }
    );
out:
    return ref_err;
}

static
void unref_driver_map(
    struct hound_ctx *ctx,
    xhash_t(DRIVER_DATA_MAP) *map,
    bool modify)
{
    struct driver *drv;
    hound_err err;
    const struct hound_data_rq_list *rq_list;
    xhiter_t iter;

    xh_iter(map, iter,
        drv = xh_key(map, iter);
        rq_list = &xh_val(map, iter);
        err = driver_unref(drv, ctx->queue, rq_list, modify);
        if (err != HOUND_OK) {
            hound_log_err(
                err,
                "ctx %p: failed to unref driver %p", (void *) ctx, (void *) drv);
        }
    );
}

static
hound_err ref_drivers(struct hound_ctx *ctx, bool modify)
{
    hound_err err;

    err = ref_driver_map(ctx, ctx->periodic_data_map, modify);
    if (err != HOUND_OK) {
        goto error_periodic;
    }

    err = ref_driver_map(ctx, ctx->on_demand_data_map, modify);
    if (err != HOUND_OK) {
        goto error_on_demand;
    }

    return HOUND_OK;

error_on_demand:
    unref_driver_map(ctx, ctx->periodic_data_map, modify);
error_periodic:
    return err;
}

static
void unref_drivers(struct hound_ctx *ctx, bool modify)
{
    unref_driver_map(ctx, ctx->periodic_data_map, modify);
    unref_driver_map(ctx, ctx->on_demand_data_map, modify);
}

static
hound_err ctx_start_nolock(struct hound_ctx *ctx, bool modify)
{
    hound_err err;

    /* We must not double-ref the drivers. */
    if (ctx->active) {
        return HOUND_CTX_ACTIVE;
    }

    err = ref_drivers(ctx, modify);
    if (err != HOUND_OK) {
        return err;
    }

    ctx->active = true;

    return HOUND_OK;
}

hound_err ctx_start(struct hound_ctx *ctx)
{
    hound_err err;

    NULL_CHECK(ctx);

    pthread_rwlock_wrlock(&ctx->rwlock);
    err = ctx_start_nolock(ctx, false);
    pthread_rwlock_unlock(&ctx->rwlock);

    return err;
}

static
hound_err ctx_stop_nolock(struct hound_ctx *ctx, bool modify)
{
    /* We must not double-unref the drivers. */
    if (!ctx->active) {
        return HOUND_CTX_NOT_ACTIVE;
    }

    queue_interrupt(ctx->queue);
    unref_drivers(ctx, modify);
    ctx->active = false;

    return HOUND_OK;
}

hound_err ctx_stop(struct hound_ctx *ctx)
{
    hound_err err;

    NULL_CHECK(ctx);

    pthread_rwlock_wrlock(&ctx->rwlock);
    err = ctx_stop_nolock(ctx, false);
    pthread_rwlock_unlock(&ctx->rwlock);

    return err;
}

static
hound_err replace_maps(
    struct hound_ctx *ctx,
    const struct hound_data_rq_list *rq_list)
{
    hound_err err;
    xhash_t(DRIVER_DATA_MAP) *periodic_map;
    xhash_t(DRIVER_DATA_MAP) *on_demand_map;

    err = make_driver_data_maps(rq_list, &periodic_map, &on_demand_map);
    if (err != HOUND_OK) {
        return err;
    }

    destroy_driver_data_map(ctx->periodic_data_map);
    destroy_driver_data_map(ctx->on_demand_data_map);

    ctx->periodic_data_map = periodic_map;
    ctx->on_demand_data_map = on_demand_map;

    return HOUND_OK;
}

hound_err ctx_modify(
    struct hound_ctx *ctx,
    const struct hound_rq *rq,
    bool flush)
{
    hound_err err;
    bool stopped;

    NULL_CHECK(ctx);
    NULL_CHECK(rq);

    err = validate_rq(rq);
    if (err != HOUND_OK) {
        return err;
    }

    /* The request is OK, so let's proceed. */
    pthread_rwlock_wrlock(&ctx->rwlock);

    if (ctx->active) {
        err = ctx_stop_nolock(ctx, true);
        if (err != HOUND_OK) {
            goto error_stop;
        }
        stopped = true;
    }
    else {
        stopped = false;
    }

    err = queue_resize(&ctx->queue, rq->queue_len, flush);
    if (err != HOUND_OK) {
        goto error_queue_resize;
    }

    err = replace_maps(ctx, &rq->rq_list);
    if (err != HOUND_OK) {
        goto error_replace_maps;
    }

    ctx->cb = rq->cb;
    ctx->cb_ctx = rq->cb_ctx;

    if (stopped) {
        err = ctx_start_nolock(ctx, true);
        if (err != HOUND_OK) {
            goto out;
        }
    }

    err = HOUND_OK;
    goto out;

error_start:
error_replace_maps:
error_queue_resize:
    if (stopped) {
        err = ctx_start_nolock(ctx, true);
        if (err != HOUND_OK) {
            hound_log_err_nofmt(err, "Failed to restart ctx");
            goto error_start;
        }
    }
error_stop:
out:
    pthread_rwlock_unlock(&ctx->rwlock);
    return err;
}

hound_err ctx_free(struct hound_ctx *ctx)
{
    bool active;
    hound_err err;
    size_t readers;

    NULL_CHECK(ctx);

    pthread_rwlock_rdlock(&ctx->rwlock);
    active = ctx->active;
    readers = ctx->readers;
    pthread_rwlock_unlock(&ctx->rwlock);
    if (active || readers > 0) {
        return HOUND_CTX_ACTIVE;
    }

    err = pthread_rwlock_destroy(&ctx->rwlock);
    XASSERT_EQ(err, 0);

    destroy_driver_data_map(ctx->periodic_data_map);
    destroy_driver_data_map(ctx->on_demand_data_map);
    queue_destroy(ctx->queue);
    free(ctx);

    return HOUND_OK;
}

static
void process_callbacks(
    struct hound_ctx *ctx,
    struct record_info **buf,
    hound_seqno seqno,
    size_t n)
{
    hound_cb cb;
    void *cb_ctx;
    size_t i;
    struct record_info *rec_info;

    if (n == 0) {
        return;
    }

    /*
     * Grab the callback and its context, since they can be changed via
     * ctx_modify.
     */
    pthread_rwlock_rdlock(&ctx->rwlock);
    cb = ctx->cb;
    cb_ctx = ctx->cb_ctx;
    pthread_rwlock_unlock(&ctx->rwlock);

    for (i = 0; i < n; ++i) {
        rec_info = buf[i];
        cb(&rec_info->record, seqno, cb_ctx);
        record_ref_dec(rec_info);
        ++seqno;
    }
}

hound_err ctx_next(struct hound_ctx *ctx, size_t n)
{
    const struct hound_data_rq *data;
    struct driver *drv;
    const struct hound_data_rq_list *rq_list;
    hound_err err;
    size_t i;
    xhiter_t iter;

    NULL_CHECK(ctx);

    pthread_rwlock_rdlock(&ctx->rwlock);
    xh_iter(ctx->on_demand_data_map, iter,
        drv = xh_key(ctx->on_demand_data_map, iter);
        rq_list = &xh_val(ctx->on_demand_data_map, iter);

        for (i = 0; i < rq_list->len; ++i) {
            data = &rq_list->data[i];
            err = driver_next(drv, data->id, n);
            if (err != HOUND_OK) {
                hound_log_err(
                    err,
                    "ctx %p: driver %p failed next() call",
                    (void *) ctx,
                    (void *) drv);
            }
        }
    );
    pthread_rwlock_unlock(&ctx->rwlock);

    return HOUND_OK;
}

static
void start_read(struct hound_ctx *ctx, struct queue **queue)
{
    /*
     * It's safe to hold onto a reference to the queue without the lock because
     * we never change the queue pointer while the context is alive, and the
     * context can't be freed while readers > 0. The context may be modified,
     * but that's all done using the queue lock and doesn't change the value of
     * ctx->queue.
     */
    pthread_rwlock_wrlock(&ctx->rwlock);
    ++ctx->readers;
    *queue = ctx->queue;
    pthread_rwlock_unlock(&ctx->rwlock);
}

static
void stop_read(struct hound_ctx *ctx)
{
    pthread_rwlock_wrlock(&ctx->rwlock);
    --ctx->readers;
    pthread_rwlock_unlock(&ctx->rwlock);
}

hound_err ctx_read(struct hound_ctx *ctx, size_t records, size_t *read)
{
    struct record_info *buf[DEQUEUE_BUF_SIZE];
    hound_err err;
    hound_seqno first_seqno;
    bool interrupt;
    size_t pop_count;
    struct queue *queue;
    size_t target;
    size_t total;

    NULL_CHECK(ctx);

    start_read(ctx, &queue);

    if (records > queue_max_len(queue)) {
        err = HOUND_QUEUE_TOO_SMALL;
        goto out;
    }

    /* Dequeue and process callbacks. */
    total = 0;
    do {
        target = min(records - total, ARRAYLEN(buf));
        pop_count = queue_pop_records_sync(
            queue,
            buf,
            target,
            &first_seqno,
            &interrupt);
        process_callbacks(ctx, buf, first_seqno, pop_count);
        total += pop_count;
    } while (total < records && !interrupt);

    if (interrupt) {
        err = HOUND_CTX_STOPPED;
    }
    else {
        err = HOUND_OK;
    }

    if (read != NULL) {
        *read = total;
    }

out:
    stop_read(ctx);
    return err;
}

/*
 * NB: There is no ctx_read_bytes function; this is deliberate. The
 * problem with such a function is that records are not guaranteed to be a fixed
 * size, and until a record is delivered, the driver does not necessarily know
 * how large the record for a given data ID will be. Thus there is no good way
 * to call driver_next for a given number of bytes.
 *
 * That said, it would be possible to implement a queue_pop_bytes_sync function.
 * You could do something like this:
 *  - add a new property to each context indicating a guess of how many bytes
 *    per sample. initially set this property to something reasonably small but
 *    arbitrary. call this property "guess".
 *  - when doing ctx_read_bytes:
 *      - sync read target bytes / "guess" number of samples.
 *      - block waiting for that read to finish
 *      - check how many bytes you got. adjust "guess" according to this, erring
 *        on the low side because you don't want to read more bytes than
 *        requested. you would want to figure out a heuristic for how to adjust
 *        "guess" (always take the most recent guess, average the new guess and
 *        the old one, or something else?)
 *      - keep reading until you're done, continually adjusting "guess" based on
 *        how many bytes are in the samples you're getting.

 * Considering that the heuristic requered is non-obvious, this is clearly
 * complex, and you risk reading more samples than requested, let's not
 * implement ctx_read_bytes until there is a clear need for it. At that point,
 * perhaps concrete requirements will make it more obvious how it should be
 * implemented.
*/

hound_err ctx_read_nowait(struct hound_ctx *ctx, size_t records, size_t *read)
{
    struct record_info *buf[DEQUEUE_BUF_SIZE];
    size_t count;
    hound_seqno first_seqno;
    struct queue *queue;
    size_t total;
    size_t target;

    NULL_CHECK(ctx);

    start_read(ctx, &queue);

    total = 0;
    do {
        target = min(records - total, ARRAYLEN(buf));
        count = queue_pop_records_nowait(queue, buf, &first_seqno, target);
        process_callbacks(ctx, buf, first_seqno, count);
        total += count;
    } while (count == target && total < records);
    *read = total;

    stop_read(ctx);

    return HOUND_OK;
}

hound_err ctx_read_bytes_nowait(
    struct hound_ctx *ctx,
    size_t bytes,
    size_t *records_read,
    size_t *bytes_read)
{
    struct record_info *buf[DEQUEUE_BUF_SIZE];
    size_t count;
    hound_seqno first_seqno;
    struct queue *queue;
    size_t records;
    size_t total_bytes;
    size_t total_records;
    size_t target;

    NULL_CHECK(ctx);

    start_read(ctx, &queue);

    total_bytes = 0;
    total_records = 0;
    do {
        target = min(bytes - total_bytes, sizeof(buf));

        count = queue_pop_bytes_nowait(
            queue,
            buf,
            target,
            &first_seqno,
            &records);
        process_callbacks(ctx, buf, first_seqno, records);

        total_records += records;
        total_bytes += count;
    } while (count == target && total_bytes < bytes);

    *bytes_read = total_bytes;
    *records_read = total_records;

    stop_read(ctx);

    return HOUND_OK;
}

hound_err ctx_read_all_nowait(struct hound_ctx *ctx, size_t *read)
{
    return ctx_read_nowait(ctx, SIZE_MAX, read);
}

hound_err ctx_queue_length(struct hound_ctx *ctx, size_t *count)
{
    NULL_CHECK(ctx);

    pthread_rwlock_rdlock(&ctx->rwlock);
    *count = queue_len(ctx->queue);
    pthread_rwlock_unlock(&ctx->rwlock);

    return HOUND_OK;
}

hound_err ctx_max_queue_length(struct hound_ctx *ctx, size_t *count)
{
    NULL_CHECK(ctx);

    pthread_rwlock_rdlock(&ctx->rwlock);
    *count = queue_max_len(ctx->queue);
    pthread_rwlock_unlock(&ctx->rwlock);

    return HOUND_OK;
}
