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

XVEC_DEFINE(data_rq_vec, struct hound_data_rq);
XVEC_DEFINE(id_vec, hound_data_id);

/* driver --> list of data needed from that driver */
XHASH_MAP_INIT_PTR(DRIVER_DATA_MAP, struct driver *, data_rq_vec) /* NOLINT */

/* driver --> list of on-demand data IDs */
XHASH_MAP_INIT_PTR(ON_DEMAND_MAP, struct driver *, id_vec) /* NOLINT */
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
    xhash_t(DRIVER_DATA_MAP) *drv_data_map;
    xhash_t(ON_DEMAND_MAP) *on_demand_data_map;
};

static
void destroy_drv_data_map(xhash_t(DRIVER_DATA_MAP) *map)
{

    xhiter_t iter;

    xh_iter(map, iter,
        xv_destroy(xh_val(map, iter));
    );
    xh_destroy(DRIVER_DATA_MAP, map);
}

static
void destroy_on_demand_map(xhash_t(ON_DEMAND_MAP) *map)
{
    xhiter_t iter;

    xh_iter(map, iter,
        xv_destroy(xh_val(map, iter));
    );
    xh_destroy(ON_DEMAND_MAP, map);
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

        if (!driver_period_supported(drv, data_rq->id, data_rq->period_ns)) {
            return HOUND_PERIOD_UNSUPPORTED;
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
    }

    return HOUND_OK;
}

static
hound_err make_driver_data_maps(
    const struct hound_data_rq_list *list,
    xhash_t(DRIVER_DATA_MAP) **out_drv_data_map,
    xhash_t(ON_DEMAND_MAP) **out_on_demand_map)
{
    const struct hound_data_rq *data_rq;
    struct driver *drv;
    xhash_t(DRIVER_DATA_MAP) *drv_data_map;
    hound_err err;
    size_t i;
    hound_data_id *id;
    id_vec *id_list;
    xhiter_t iter;
    struct hound_data_rq *new_rq;
    xhash_t(ON_DEMAND_MAP) *on_demand_map;
    int ret;
    data_rq_vec *rq_vec;

    drv_data_map = xh_init(DRIVER_DATA_MAP);
    if (drv_data_map == NULL) {
        err = HOUND_OOM;
        goto error_drv_data_map;
    }

    on_demand_map = xh_init(ON_DEMAND_MAP);
    if (on_demand_map == NULL) {
        err = HOUND_OOM;
        goto error_on_demand_map;
    }

    /*
     * Create driver entries in each map and count how many items we'll need in
     * the driver lists inside the maps.
     */
    for (i = 0; i < list->len; ++i) {
        data_rq = &list->data[i];

        err = driver_get(data_rq->id, &drv);
        if (err != HOUND_OK) {
            goto error_loop;
        }

        iter = xh_put(DRIVER_DATA_MAP, drv_data_map, drv, &ret);
        if (ret == -1) {
            err = HOUND_OOM;
            goto error_loop;
        }
        rq_vec = &xh_val(drv_data_map, iter);

        if (ret != 0) {
            /* The driver was newly added to the map. */
            xv_init(*rq_vec);
        }

        new_rq = xv_pushp(struct hound_data_rq, *rq_vec);
        if (new_rq == NULL) {
            goto error_loop;
        }
        *new_rq = *data_rq;

        if (data_rq->period_ns == 0) {
            /* On-demand data. */
            iter = xh_put(ON_DEMAND_MAP, on_demand_map, drv, &ret);
            if (ret == -1) {
                err = HOUND_OOM;
                goto error_loop;
            }
            id_list = &xh_val(on_demand_map, iter);

            if (ret != 0) {
                /* The driver was newly added to the map. */
                xv_init(*id_list);
            }

            id = xv_pushp(hound_data_id, *id_list);
            if (id == NULL) {
                goto error_loop;
            }
            *id = data_rq->id;
        }
    }

    xh_trim(DRIVER_DATA_MAP, drv_data_map);
    xh_trim(ON_DEMAND_MAP, on_demand_map);

    xh_iter(drv_data_map, iter,
        xv_trim(struct hound_data_rq, xh_val(drv_data_map, iter));
    );
    xh_iter(on_demand_map, iter,
        xv_trim(hound_data_id, xh_val(on_demand_map, iter));
    );

    *out_drv_data_map = drv_data_map;
    *out_on_demand_map = on_demand_map;

    return HOUND_OK;

error_loop:
    destroy_drv_data_map(drv_data_map);
error_on_demand_map:
    destroy_on_demand_map(on_demand_map);
error_drv_data_map:
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
        &ctx->drv_data_map,
        &ctx->on_demand_data_map);
    if (err != HOUND_OK) {
        goto error_make_data_maps;
    }

    *ctx_out = ctx;
    return HOUND_OK;

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
hound_err ref_drivers(struct hound_ctx *ctx)
{
    struct driver *drv;
    hound_err ref_err;
    xhiter_t ref_iter;
    data_rq_vec *rq_vec;
    hound_err unref_err;
    xhiter_t unref_iter;

    xh_iter(ctx->drv_data_map, ref_iter,
        drv = xh_key(ctx->drv_data_map, ref_iter);
        rq_vec = &xh_val(ctx->drv_data_map, ref_iter);
        ref_err = driver_ref(
            drv,
            ctx->queue,
            xv_data(*rq_vec),
            xv_size(*rq_vec));
        if (ref_err != HOUND_OK) {
            goto error;
        }
    );

    ref_err = HOUND_OK;
    goto out;

error:
    /* Unref any drivers we reffed. */
    xh_iter(ctx->drv_data_map, unref_iter,
        /* We have hit the the driver that failed to ref, so stop here. */
        if (unref_iter == ref_iter) {
            break;
        }

        drv = xh_key(ctx->drv_data_map, unref_iter);
        rq_vec = &xh_val(ctx->drv_data_map, unref_iter);
        unref_err = driver_unref(
            drv,
            ctx->queue,
            xv_data(*rq_vec),
            xv_size(*rq_vec));
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
hound_err unref_drivers(struct hound_ctx *ctx)
{
    struct driver *drv;
    hound_err err;
    data_rq_vec *rq_vec;
    xhiter_t iter;
    hound_err tmp;

    err = HOUND_OK;
    xh_iter(ctx->drv_data_map, iter,
        drv = xh_key(ctx->drv_data_map, iter);
        rq_vec = &xh_val(ctx->drv_data_map, iter);
        tmp = driver_unref(
            drv,
            ctx->queue,
            xv_data(*rq_vec),
            xv_size(*rq_vec));
        if (tmp != HOUND_OK) {
            err = tmp;
            hound_log_err(
                err,
                "ctx %p: failed to unref driver %p", (void *) ctx, (void *) drv);
        }
    );

    return err;
}

hound_err modify_drivers(
    struct hound_ctx *ctx,
    xhash_t(DRIVER_DATA_MAP) *new_drv_data_map)
{
    struct driver *drv;
    hound_err err;
    xhiter_t iter;
    xhiter_t new_iter;
    xhiter_t old_iter;
    data_rq_vec *new_rq_vec;
    data_rq_vec *old_rq_vec;
    hound_err tmp;

    /*
     * First go through the new map and modify/ref each driver, depending on
     * whether or not it overlaps with the old map.
     */
    xh_iter(new_drv_data_map, new_iter,
        drv = xh_key(new_drv_data_map, new_iter);
        new_rq_vec = &xh_val(new_drv_data_map, new_iter);

        old_iter = xh_get(DRIVER_DATA_MAP, ctx->drv_data_map, drv);
        if (old_iter != xh_end(ctx->drv_data_map)) {
            /*
             * This driver is also in the old driver map, so we can just modify
             * it.
             */
            old_rq_vec = &xh_val(ctx->drv_data_map, new_iter);
            err = driver_modify(
                drv,
                ctx->queue,
                xv_data(*old_rq_vec),
                xv_size(*old_rq_vec),
                xv_data(*new_rq_vec),
                xv_size(*new_rq_vec));
        }
        else {
            /*
             * This driver is not in the old driver map, so we have to ref it if
             * the context is currently active (if not active, it will get
             * reffed when the context is started).
             */
            if (ctx->active) {
                err = driver_ref(
                    drv,
                    ctx->queue,
                    xv_data(*new_rq_vec),
                    xv_size(*new_rq_vec));
            }
        }
        if (err != HOUND_OK) {
            goto error_new_vec;
        }
    );

    /* Next, go through the old map and unref any drivers not in the new map. */
    xh_iter(ctx->drv_data_map, old_iter,
        if (xh_exist(new_drv_data_map, old_iter)) {
            continue;
        }

        drv = xh_key(ctx->drv_data_map, old_iter);
        old_rq_vec = &xh_val(ctx->drv_data_map, old_iter);
        err = driver_unref(
            drv,
            ctx->queue,
            xv_data(*old_rq_vec),
            xv_size(*old_rq_vec));
        if (err != HOUND_OK) {
            if (err != HOUND_OK) {
                goto error_unref;
            }
        }
    );

    return HOUND_OK;

error_unref:
    xh_iter(ctx->drv_data_map, iter,
        if (iter == old_iter) {
            break;
        }
        if (xh_exist(new_drv_data_map, iter)) {
            continue;
        }

        drv = xh_key(ctx->drv_data_map, iter);
        old_rq_vec = &xh_val(ctx->drv_data_map, iter);
        err = driver_ref(
            drv,
            ctx->queue,
            xv_data(*old_rq_vec),
            xv_size(*old_rq_vec));
        if (err != HOUND_OK) {
            hound_log_err(
                err,
                "failed to ref driver %p during cleanup",
                (void *) drv);
        }
    );
error_new_vec:
    xh_iter(new_drv_data_map, iter,
        if (iter == new_iter) {
            break;
        }
        drv = xh_key(new_drv_data_map, iter);
        new_rq_vec = &xh_val(new_drv_data_map, new_iter);

        old_iter = xh_get(DRIVER_DATA_MAP, ctx->drv_data_map, drv);
        if (old_iter == xh_end(ctx->drv_data_map)) {
            old_rq_vec = &xh_val(ctx->drv_data_map, new_iter);
            tmp = driver_modify(
                drv,
                ctx->queue,
                xv_data(*new_rq_vec),
                xv_size(*new_rq_vec),
                xv_data(*old_rq_vec),
                xv_size(*old_rq_vec));
                if (tmp != HOUND_OK) {
                    hound_log_err(
                        err,
                        "failed to modify driver %p during cleanup",
                        (void *) drv);
                        }
        }
        else {
            if (ctx->active) {
                tmp = driver_unref(
                    drv,
                    ctx->queue,
                    xv_data(*new_rq_vec),
                    xv_size(*new_rq_vec));
                if (tmp != HOUND_OK) {
                    hound_log_err(
                        err,
                        "failed to unref driver %p during cleanup",
                        (void *) drv);
                }
            }
        }
    );
    return err;
}

static
hound_err ctx_start_nolock(struct hound_ctx *ctx)
{
    hound_err err;

    /* We must not double-ref the drivers. */
    if (ctx->active) {
        return HOUND_CTX_ACTIVE;
    }

    err = ref_drivers(ctx);
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
    err = ctx_start_nolock(ctx);
    pthread_rwlock_unlock(&ctx->rwlock);

    return err;
}

static
hound_err ctx_stop_nolock(struct hound_ctx *ctx)
{
    hound_err err;

    /* We must not double-unref the drivers. */
    if (!ctx->active) {
        return HOUND_CTX_NOT_ACTIVE;
    }

    queue_interrupt(ctx->queue);
    err = unref_drivers(ctx);
    ctx->active = false;

    return err;
}

hound_err ctx_stop(struct hound_ctx *ctx)
{
    hound_err err;

    NULL_CHECK(ctx);

    pthread_rwlock_wrlock(&ctx->rwlock);
    err = ctx_stop_nolock(ctx);
    pthread_rwlock_unlock(&ctx->rwlock);

    return err;
}

hound_err ctx_modify(
    struct hound_ctx *ctx,
    const struct hound_rq *rq,
    bool flush)
{
    xhash_t(DRIVER_DATA_MAP) *drv_data_map;
    hound_err err;
    xhash_t(ON_DEMAND_MAP) *on_demand_map;
    size_t orig_max_len;
    hound_err tmp;

    NULL_CHECK(ctx);
    NULL_CHECK(rq);

    err = validate_rq(rq);
    if (err != HOUND_OK) {
        return err;
    }

    /* The request is OK, so let's proceed. */
    pthread_rwlock_wrlock(&ctx->rwlock);

    orig_max_len = queue_max_len(ctx->queue);
    err = queue_resize(ctx->queue, rq->queue_len, flush);
    if (err != HOUND_OK) {
        goto error_resize;
    }

    err = make_driver_data_maps(&rq->rq_list, &drv_data_map, &on_demand_map);
    if (err != HOUND_OK) {
        goto error_driver_maps;
    }

    err = modify_drivers(ctx, drv_data_map);
    if (err != HOUND_OK) {
        goto error_modify_drivers;
    }

    destroy_drv_data_map(ctx->drv_data_map);
    destroy_on_demand_map(ctx->on_demand_data_map);

    ctx->drv_data_map = drv_data_map;
    ctx->on_demand_data_map = on_demand_map;
    ctx->cb = rq->cb;
    ctx->cb_ctx = rq->cb_ctx;

    err = HOUND_OK;
    goto out;

error_modify_drivers:
    destroy_drv_data_map(drv_data_map);
    destroy_on_demand_map(on_demand_map);
error_driver_maps:
    tmp = queue_resize(ctx->queue, orig_max_len, false);
    if (tmp != HOUND_OK) {
        hound_log_err(
            err,
            "failed to restore queue for ctx %p to its original size of %zu; size is now %lu",
            (void *) ctx,
            orig_max_len,
            rq->queue_len);
    }
error_resize:
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

    destroy_drv_data_map(ctx->drv_data_map);
    destroy_on_demand_map(ctx->on_demand_data_map);
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
    struct driver *drv;
    hound_err err;
    size_t i;
    id_vec *ids;
    xhiter_t iter;

    NULL_CHECK(ctx);

    err = HOUND_OK;

    pthread_rwlock_rdlock(&ctx->rwlock);
    xh_iter(ctx->on_demand_data_map, iter,
        drv = xh_key(ctx->on_demand_data_map, iter);
        ids = &xh_val(ctx->on_demand_data_map, iter);

        for (i = 0; i < xv_size(*ids); ++i) {
            err = driver_next(drv, xv_A(*ids, i), n);
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

    return err;
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
        pop_count = queue_pop_records(
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
 * That said, it would be possible to implement a blocking queue_pop_bytes
 * function.  You could do something like this:
 *  - add a new property to each context indicating a guess of how many bytes
 *    per sample. initially set this property to something reasonably small but
 *    arbitrary. call this property "guess".
 *  - when doing ctx_read_bytes:
 *      - read target bytes / "guess" number of samples.
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
