/**
 * @file      ctx.c
 * @brief     Hound context tracking subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _GNU_SOURCE
#include <hound/error.h>
#include <hound/hound.h>
#include <hound/log.h>
#include <hound_private/driver.h>
#include <hound_private/util.h>
#include <pthread.h>
#include <stdbool.h>
#include <xlib/xhash.h>
#include <xlib/xvec.h>

#define DEQUEUE_BUF_SIZE (128)

/* TODO: add a pointer-based hash so we don't have to do casting? */

/* driver --> list of data needed from that driver */
XHASH_MAP_INIT_INT64(DRIVER_DATA_MAP, struct hound_drv_data_list)

struct hound_ctx {
    pthread_rwlock_t rwlock;

    bool active;
    hound_cb cb;
    void *cb_ctx;
    struct queue *queue;
    xhash_t(DRIVER_DATA_MAP) *periodic_data_map;
    xhash_t(DRIVER_DATA_MAP) *on_demand_data_map;
};

void free_driver_data_map(xhash_t(DRIVER_DATA_MAP) *map)
{
    struct hound_drv_data_list *drv_data_list;
    xhiter_t iter;

    xh_iter(map, iter,
        drv_data_list = &xh_val(map, iter);
        free(drv_data_list->data);
    );
    xh_destroy(DRIVER_DATA_MAP, map);
}

hound_err ctx_alloc(struct hound_ctx **ctx_out, const struct hound_rq *rq)
{
    struct hound_ctx *ctx;
    struct hound_data_rq *data_rq;
    struct driver *drv;
    struct driver *drv_iter;
    struct hound_drv_data_list *drv_data_list;
    hound_err err;
    size_t i;
    size_t j;
    size_t index;
    xhiter_t iter;
    xhash_t(DRIVER_DATA_MAP) *map;
    size_t matches;
    const struct hound_data_rq_list *list;
    int ret;

    NULL_CHECK(ctx_out);
    NULL_CHECK(rq);

    /* Is the request sane? */
    if (rq->queue_len == 0) {
        err = HOUND_EMPTY_QUEUE;
        goto out;
    }

    list = &rq->rq_list;
    if (list->len == 0) {
        err = HOUND_NO_DATA_REQUESTED;
        goto out;
    }

    NULL_CHECK(list->data);

    if (rq->cb == NULL) {
        err = HOUND_MISSING_CALLBACK;
        goto out;
    }

    /* Are the data IDs all valid? */
    for (i = 0; i < list->len; ++i) {
        data_rq = &list->data[i];
        err = driver_get(data_rq->id, &drv);
        if (err != HOUND_OK) {
            goto out;
        }
        if (!driver_period_supported(drv, data_rq->id, data_rq->period_ns)) {
            err = HOUND_PERIOD_UNSUPPORTED;
            goto out;
        }
    }

    /* Request is fine. Let's make a new context. */
    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    err = pthread_rwlock_init(&ctx->rwlock, NULL);
    if (err != 0) {
        goto out;
    }

    ctx->active = false;
    ctx->cb = rq->cb;
    ctx->cb_ctx = rq->cb_ctx;

    err = queue_alloc(&ctx->queue, rq->queue_len);
    if (err != HOUND_OK) {
        goto error_queue_alloc;
    }

    /* Populate our context. */
    ctx->periodic_data_map = xh_init(DRIVER_DATA_MAP);
    if (ctx->periodic_data_map == NULL) {
        goto error_periodic_data_map;
    }

    ctx->on_demand_data_map = xh_init(DRIVER_DATA_MAP);
    if (ctx->on_demand_data_map == NULL) {
        goto error_on_demand_data_map;
    }

    for (i = 0; i < list->len; ++i) {
        data_rq = &list->data[i];

        err = driver_get(data_rq->id, &drv);
        if (err != HOUND_OK) {
            goto error_ctx_loop;
        }

        /*
         * We want to process all the entries for a given driver all at once, so
         * if we've already seen this driver, just continue on.
         */
        if (xh_found(DRIVER_DATA_MAP, ctx->periodic_data_map, (uint64_t) drv) ||
            xh_found(DRIVER_DATA_MAP, ctx->on_demand_data_map, (uint64_t) drv)) {
            continue;
        }

        /*
         * This is the first time we've seen this driver. Add an entry to the
         * map and populate it with all the data corresponding to this driver.
         */
        if (data_rq->period_ns != 0) {
            map = ctx->periodic_data_map;
        }
        else {
            map = ctx->on_demand_data_map;
        }
        iter = xh_put(
            DRIVER_DATA_MAP,
            map,
            (uint64_t) drv,
            &ret);
        if (ret == -1) {
            err = HOUND_OOM;
            goto error_ctx_loop;
        }
        drv_data_list = &xh_val(map, iter);

        /* Look ahead to count how many requests match this driver. */
        matches = 1;
        for (j = i+1; j < list->len; ++j) {
            err = driver_get(list->data[j].id, &drv_iter);
            if (err != HOUND_OK) {
                goto error_ctx_loop;
            }
            if (drv_iter == drv) {
                ++matches;
            }
        }
        drv_data_list->len = matches;
        drv_data_list->data = malloc(
            drv_data_list->len * sizeof(*drv_data_list->data));
        if (drv_data_list->data == NULL) {
            err = HOUND_OOM;
            goto error_ctx_loop;
        }

        /* Now populate the request list. */
        index = 0;
        for (j = i; j < list->len; ++j) {
            err = driver_get(list->data[j].id, &drv_iter);
            if (err != HOUND_OK) {
                goto error_ctx_loop;
            }
            if (drv_iter != drv) {
                continue;
            }
            drv_data_list->data[index].id = list->data[j].id;
            drv_data_list->data[index].period_ns = list->data[j].period_ns;
            ++index;
        }
    }
    xh_trim(DRIVER_DATA_MAP, ctx->periodic_data_map);
    xh_trim(DRIVER_DATA_MAP, ctx->on_demand_data_map);

    *ctx_out = ctx;
    err = HOUND_OK;
    goto out;

error_ctx_loop:
    queue_destroy(ctx->queue);
    free_driver_data_map(ctx->on_demand_data_map);
error_on_demand_data_map:
    free_driver_data_map(ctx->periodic_data_map);
error_periodic_data_map:
error_queue_alloc:
    free(ctx);
out:
    return err;
}

static
void free_record_info(struct record_info *info)
{
    free(info->record.data);
    free(info);
}

static
void destroy_cb_queue(struct queue *queue)
{
    refcount_val count;
    size_t i;
    struct record_info *buf[DEQUEUE_BUF_SIZE];
    struct record_info *rec_info;
    size_t read;

    /* First drain the queue. */
    read = queue_pop_async(queue, buf, SIZE_MAX);
    for (i = 0; i < read; ++i) {
        rec_info = buf[i];
        count = atomic_ref_dec(&rec_info->refcount);
        if (count == 1) {
            free_record_info(rec_info);
        }
    }

    /* Then destroy the queue. */
    queue_destroy(queue);
}

hound_err ctx_free(struct hound_ctx *ctx)
{
    hound_err err;

    NULL_CHECK(ctx);

    err = pthread_rwlock_destroy(&ctx->rwlock);
    HOUND_ASSERT_EQ(err, 0);

    free_driver_data_map(ctx->periodic_data_map);
    free_driver_data_map(ctx->on_demand_data_map);
    destroy_cb_queue(ctx->queue);
    free(ctx);

    return HOUND_OK;
}

hound_err ref_driver_map(struct hound_ctx *ctx, xhash_t(DRIVER_DATA_MAP) *map)
{

    struct driver *drv;
    const struct hound_drv_data_list *drv_data_list;
    xhiter_t iter;
    hound_err ref_err;
    hound_err unref_err;

    xh_iter(map, iter,
        drv = (__typeof__(drv)) xh_key(map, iter);
        drv_data_list = &xh_val(map, iter);
        ref_err = driver_ref(drv, ctx->queue, drv_data_list);
        if (ref_err != HOUND_OK) {
            --iter;
            goto error;
        }
    );

    ref_err = HOUND_OK;
    goto out;

error:
    /* Unref any drivers we reffed. */
    for (; iter < xh_end(map); --iter) {
        drv = (__typeof__(drv)) xh_key(map, iter);
        drv_data_list = &xh_val(map, iter);
        unref_err = driver_unref(drv, ctx->queue, drv_data_list);
        if (unref_err != HOUND_OK) {
            hound_log_err(
                unref_err,
                "ctx %p: failed to unref driver %p", (void *) ctx, (void *) drv);
        }
    }
out:
    return ref_err;
}

hound_err ref_drivers(struct hound_ctx *ctx)
{
    hound_err err;

    err = ref_driver_map(ctx, ctx->periodic_data_map);
    if (err != HOUND_OK) {
        return err;
    }

    err = ref_driver_map(ctx, ctx->on_demand_data_map);
    if (err != HOUND_OK) {
        return err;
    }

    return HOUND_OK;
}

void unref_driver_map(struct hound_ctx *ctx, xhash_t(DRIVER_DATA_MAP) *map)
{
    struct driver *drv;
    hound_err err;
    const struct hound_drv_data_list *drv_data_list;
    xhiter_t iter;

    xh_iter(map, iter,
        drv = (__typeof__(drv)) xh_key(map, iter);
        drv_data_list = &xh_val(map, iter);
        err = driver_unref(drv, ctx->queue, drv_data_list);
        if (err != HOUND_OK) {
            hound_log_err(
                err,
                "ctx %p: failed to unref driver %p", (void *) ctx, (void *) drv);
        }
    );
}

void unref_drivers(struct hound_ctx *ctx)
{
    unref_driver_map(ctx, ctx->periodic_data_map);
    unref_driver_map(ctx, ctx->on_demand_data_map);
}

hound_err ctx_start(struct hound_ctx *ctx)
{
    hound_err err;

    NULL_CHECK(ctx);

    pthread_rwlock_wrlock(&ctx->rwlock);

    /* We must not double-ref the drivers. */
    if (ctx->active) {
        err = HOUND_CTX_ALREADY_ACTIVE;
        goto out;
    }

    err = ref_drivers(ctx);
    if (err != HOUND_OK) {
        goto out;
    }

    ctx->active = true;

    err = HOUND_OK;
    goto out;

out:
    pthread_rwlock_unlock(&ctx->rwlock);
    return err;
}

hound_err ctx_stop(struct hound_ctx *ctx)
{
    hound_err err;

    NULL_CHECK(ctx);

    pthread_rwlock_wrlock(&ctx->rwlock);

    /* We must not double-ref the drivers. */
    if (!ctx->active) {
        err = HOUND_CTX_NOT_ACTIVE;
        goto out;
    }

    unref_drivers(ctx);
    ctx->active = false;

    err = HOUND_OK;
    goto out;

out:
    pthread_rwlock_unlock(&ctx->rwlock);
    return err;
}

static
void process_callbacks(
    struct hound_ctx *ctx,
    struct record_info **buf,
    size_t n)
{
    refcount_val count;
    size_t i;
    struct record_info *rec_info;

    for (i = 0; i < n; ++i) {
        rec_info = buf[i];
        ctx->cb(&rec_info->record, ctx->cb_ctx);
        count = atomic_ref_dec(&rec_info->refcount);
        if (count == 1) {
            /*
             * atomic_ref_dec returns the value *before* decrement, so this
             * means the refcount has now reached 0.
             */
            free_record_info(rec_info);
        }
    }
}

void ctx_next_nolock_single(struct hound_ctx *ctx)
{
    const struct hound_drv_data *data;
    struct driver *drv;
    const struct hound_drv_data_list *drv_data_list;
    hound_err err;
    size_t i;
    xhiter_t iter;

    xh_iter(ctx->on_demand_data_map, iter,
        drv = (__typeof__(drv)) xh_key(ctx->on_demand_data_map, iter);
        drv_data_list = &xh_val(ctx->on_demand_data_map, iter);

        for (i = 0; i < drv_data_list->len; ++i) {
            data = &drv_data_list->data[i];
            err = driver_next(drv, data->id);
            if (err != HOUND_OK) {
                hound_log_err(
                    err,
                    "ctx %p: driver %p failed next() call", (void *) ctx, (void *) drv);
            }
        }
    );
}

void ctx_next_nolock(struct hound_ctx *ctx, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        ctx_next_nolock_single(ctx);
    }
}

hound_err ctx_next(struct hound_ctx *ctx, size_t n)
{
    NULL_CHECK(ctx);

    pthread_rwlock_rdlock(&ctx->rwlock);
    ctx_next_nolock(ctx, n);
    pthread_rwlock_unlock(&ctx->rwlock);

    return HOUND_OK;
}

hound_err ctx_read(struct hound_ctx *ctx, size_t n)
{
    struct record_info *buf[DEQUEUE_BUF_SIZE];
    hound_err err;
    size_t target;
    size_t total;

    NULL_CHECK(ctx);

    pthread_rwlock_rdlock(&ctx->rwlock);

    if (n > queue_max_len(ctx->queue)) {
        err = HOUND_QUEUE_TOO_SMALL;
        goto out;
    }

    /* Request data from any on-demand data types. */
    ctx_next_nolock(ctx, n);

    /* Dequeue and process callbacks. */
    total = 0;
    do {
        target = min(n - total, ARRAYLEN(buf));
        queue_pop(ctx->queue, buf, target);
        process_callbacks(ctx, buf, target);
        total += target;
    } while (total < n);

    err = HOUND_OK;

out:
    pthread_rwlock_unlock(&ctx->rwlock);
    return err;
}

hound_err ctx_read_async(struct hound_ctx *ctx, size_t n, size_t *read)
{
    struct record_info *buf[DEQUEUE_BUF_SIZE];
    size_t count;
    size_t total;
    size_t target;

    NULL_CHECK(ctx);

    pthread_rwlock_rdlock(&ctx->rwlock);

    total = 0;
    do {
        if (n - total < ARRAYLEN(buf)) {
            target = n - total;
        }
        else {
            target = ARRAYLEN(buf);
        }

        count = queue_pop_async(ctx->queue, buf, target);
        process_callbacks(ctx, buf, count);
        total += count;
    } while (count == target && total < n);
    *read = total;

    pthread_rwlock_unlock(&ctx->rwlock);

    return HOUND_OK;
}

hound_err ctx_read_all(struct hound_ctx *ctx, size_t *read)
{
    return ctx_read_async(ctx, SIZE_MAX, read);
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
