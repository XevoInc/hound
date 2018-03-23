/**
 * @file      driver.c
 * @brief     Hound driver subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _GNU_SOURCE
#include <hound/hound.h>
#include <hound_private/api.h>
#include <hound_private/driver.h>
#include <hound_private/driver_ops.h>
#include <hound_private/driver/util.h>
#include <hound_private/error.h>
#include <hound_private/io.h>
#include <hound_private/log.h>
#include <hound_private/util.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xlib/xhash.h>
#include <xlib/xvec.h>

#define FD_INVALID (-1)

/* device path --> driver ID */
XHASH_MAP_INIT_STR(DEVICE_MAP, struct driver *)
static xhash_t(DEVICE_MAP) *s_device_map = NULL;

/* data ID --> data info */
XHASH_MAP_INIT_INT64(DATA_MAP, struct driver *)
static xhash_t(DATA_MAP) *s_data_map;

static pthread_rwlock_t s_driver_rwlock = PTHREAD_RWLOCK_INITIALIZER;

PUBLIC_API
void *drv_alloc(size_t bytes)
{
    return malloc(bytes);
}

PUBLIC_API
void drv_free(void *p)
{
    free(p);
}

PUBLIC_API
void *drv_ctx(void)
{
    const struct driver *drv;

    /*
     * This should be called only from a driver's callback, so we should already
     * hold the driver's mutex.
     */
    drv = get_active_drv();
    XASSERT_NOT_NULL(drv);
    return drv->ctx;
}

PUBLIC_API
void drv_set_ctx(void *ctx)
{
    struct driver *drv;

    /*
     * This should be called only from a driver's callback, so we should already
     * hold the driver's mutex.
     */
    drv = get_active_drv();
    XASSERT_NOT_NULL(drv);
    drv->ctx = ctx;
}

void driver_init(void)
{
    s_data_map = xh_init(DATA_MAP);
    XASSERT_NOT_NULL(s_data_map);
    s_device_map = xh_init(DEVICE_MAP);
    XASSERT_NOT_NULL(s_device_map);
    driver_ops_init();
}

void driver_destroy(void)
{
    driver_ops_destroy();
    xh_destroy(DEVICE_MAP, s_device_map);
    xh_destroy(DATA_MAP, s_data_map);
}

hound_err driver_get_datadesc(struct hound_datadesc **desc, size_t *len)
{
    struct driver *drv;
    hound_err err;
    const struct hound_datadesc *pos;
    size_t size;

    NULL_CHECK(len);
    NULL_CHECK(desc);

    pthread_rwlock_rdlock(&s_driver_rwlock);

    /* Find the required data size. */
    size = 0;
    xh_foreach_value(s_device_map, drv,
        size += drv->datacount;
    );

    *len = size;
    if (size == 0) {
        /*
         * If there's no data, set desc to NULL to help protect caller's against
         * freeing random memory when they call hound_free_datadesc to cleanup.
         */
        *desc = NULL;
        err = HOUND_OK;
        goto out;
    }
    else {
        /* Allocate. */
        *desc = malloc(size*sizeof(**desc));
        if (*desc == NULL) {
            err = HOUND_OOM;
            goto out;
        }
    }

    pos = *desc;
    xh_foreach_value(s_device_map, drv,
        memcpy((void *) pos, drv->data, drv->datacount*sizeof(*drv->data));
        pos += drv->datacount;
    );

    err = HOUND_OK;

out:
    pthread_rwlock_unlock(&s_driver_rwlock);
    return err;
}

void driver_free_datadesc(struct hound_datadesc *desc)
{
    /*
     * Note: This frees the user-facing data descriptor but not the descriptor
     * allocated by the driver, which we store and free only when the driver
     * unregisters.
     */
    free(desc);
}

PUBLIC_API
hound_err driver_register(
    const char *path,
    struct driver_ops *ops,
    void *init_data)
{
    struct driver *drv;
    hound_err err;
    size_t i;
    xhiter_t iter;
    int ret;

    NULL_CHECK(path);

    pthread_rwlock_wrlock(&s_driver_rwlock);

    iter = xh_get(DEVICE_MAP, s_device_map, path);
    if (iter != xh_end(s_device_map)) {
        err = HOUND_DRIVER_ALREADY_REGISTERED;
        goto out;
    }

    NULL_CHECK(ops);
    NULL_CHECK(ops->init);
    NULL_CHECK(ops->destroy);
    NULL_CHECK(ops->reset);
    NULL_CHECK(ops->device_id);
    NULL_CHECK(ops->datadesc);
    NULL_CHECK(ops->setdata);
    NULL_CHECK(ops->parse);
    NULL_CHECK(ops->start);
    NULL_CHECK(ops->next);
    NULL_CHECK(ops->stop);

    /* Allocate. */
    drv = malloc(sizeof(*drv));
    if (drv == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    /* Initialize driver fields. */
    pthread_mutex_init(&drv->mutex, NULL);
    drv->refcount = 0;
    drv->fd = FD_INVALID;
    xv_init(drv->active_data);
    drv->ops = *ops;
    drv->ctx = NULL;

    /* Init. */
    drv->ctx = NULL;
    err = drv_op_init(drv, init_data);
    if (err != HOUND_OK) {
        goto error_init;
    }

    /* Device IDs. */
    err = drv_op_device_id(drv, drv->device_id);
    if (err != HOUND_OK) {
        goto error_device_id;
    }

    if (strnlen(drv->device_id, HOUND_DEVICE_ID_MAX) == HOUND_DEVICE_ID_MAX) {
        err = HOUND_INVALID_STRING;
        goto error_device_id;
    }

    /* Get all the supported data for the driver. */
    err = drv_op_datadesc(drv, &drv->data, &drv->datacount);
    if (err != HOUND_OK) {
        goto error_datadesc;
    }

    for (i = 0; i < drv->datacount; ++i) {
        if (drv->data[i].name == NULL) {
            err = HOUND_NULL_VAL;
            goto error_datadesc;
        }
        if (strnlen(drv->data[i].name, HOUND_DEVICE_NAME_MAX) ==
            HOUND_DEVICE_NAME_MAX) {
            err = HOUND_INVALID_STRING;
            goto error_datadesc;
        }
    }

    /* Verify that all descriptors are sane. */
    for (i = 0; i < drv->datacount; ++i) {
        iter = xh_get(DATA_MAP, s_data_map, drv->data[i].id);
        if (iter != xh_end(s_data_map)) {
            err = HOUND_CONFLICTING_DRIVERS;
            goto error_conflicting_drivers;
        }
    }

    /*
     * Finally, commit the driver into all the maps.
     */
    iter = xh_put(DEVICE_MAP, s_device_map, path, &ret);
    if (ret == -1) {
        err = HOUND_OOM;
        goto error_device_map_put;
    }
    xh_val(s_device_map, iter) = drv;

    for (i = 0; i < drv->datacount; ++i ) {
        iter = xh_put(DATA_MAP, s_data_map, drv->data[i].id, &ret);
        if (ret == -1) {
            err = HOUND_OOM;
            goto error_data_map_put;
        }
        xh_val(s_data_map, iter) = drv;
    }

    err = HOUND_OK;
    goto out;

error_data_map_put:
    iter = xh_get(DEVICE_MAP, s_device_map, path);
    XASSERT_NEQ(iter, xh_end(s_device_map));
    xh_del(DEVICE_MAP, s_device_map, iter);
error_device_map_put:
error_conflicting_drivers:
error_datadesc:
error_device_id:
error_init:
    free(drv);
out:
    pthread_rwlock_unlock(&s_driver_rwlock);
    return err;
}

hound_err driver_unregister(const char *path)
{
    struct driver *drv;
    struct driver *drv_iter;
    hound_err err;
    size_t i;
    xhiter_t iter;

    NULL_CHECK(path);

    pthread_rwlock_wrlock(&s_driver_rwlock);

    /* Make sure the driver is actually registered. */
    iter = xh_get(DEVICE_MAP, s_device_map, path);
    if (iter == xh_end(s_device_map)) {
        err = HOUND_DRIVER_NOT_REGISTERED;
        goto out;
    }
    drv = xh_val(s_device_map, iter);

    /* Make sure the driver is not in-use. */
    if (drv->refcount != 0) {
        err = HOUND_DRIVER_IN_USE;
        goto out;
    }

    /* Remove the driver from all the maps so no one can access it. */
    xh_foreach_value_iter(s_device_map, drv_iter, iter,
        if (drv_iter == drv) {
            xh_del(DEVICE_MAP, s_device_map, iter);
        }
    );

    /* Remove the driver from the data map for each datatype it manages. */
    xh_foreach_value_iter(s_data_map, drv_iter, iter,
        if (drv_iter == drv) {
            xh_del(DATA_MAP, s_data_map, iter);
        }
    );

    pthread_rwlock_unlock(&s_driver_rwlock);

    /* Finally, stop and free the driver. */
    err = drv_op_destroy(drv);
    if (err != HOUND_OK) {
        hound_log_err(err, "driver %p failed to destroy", (void *) drv);
    }

    /* Free the driver-allocated data descriptor. */
    for (i = 0; i < drv->datacount; ++i) {
        drv_destroy_desc(&drv->data[i]);
    }
    free(drv->data);

    err = pthread_mutex_destroy(&drv->mutex);
    XASSERT_EQ(err, 0);
    xv_destroy(drv->active_data);
    free(drv);

    err = HOUND_OK;

out:
    return err;
}

static
size_t get_active_data_index(
    const struct driver *drv,
    const struct hound_data_rq *drv_data,
    bool *found)
{
    const struct data *data;
    size_t i;

    for (i = 0; i < xv_size(drv->active_data); ++i) {
        data = &xv_A(drv->active_data, i);
        if (data->data->id == drv_data->id) {
            *found = true;
            return i;
        }
    }

    *found = false;
    return SIZE_MAX;
}

static
hound_err push_drv_data(struct driver *drv, struct hound_data_rq *drv_data)
{
    struct data *data;

    data = xv_pushp(struct data, drv->active_data);
    if (xv_data(drv->active_data) == NULL) {
        hound_log_err(
            HOUND_OOM,
            "Failed to push drv data onto active data list",
            drv_data->id);
        return HOUND_OOM;
    }
    data->refcount = 1;
    data->data = drv_data;

    return HOUND_OK;
}


hound_err driver_next(struct driver *drv, hound_data_id id, size_t n)
{
    hound_err err;
    size_t i;

    XASSERT_NOT_NULL(drv);

    pthread_mutex_lock(&drv->mutex);
    for (i = 0; i < n; ++i) {
        err = drv_op_next(drv, id);
        if (err != HOUND_OK) {
            goto out;
        }
    }
    pthread_mutex_unlock(&drv->mutex);
    err = HOUND_OK;

out:
    return err;
}

hound_err driver_ref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *drv_data_list)
{
    bool changed;
    struct data *data;
    struct hound_data_rq *drv_data;
    hound_err err;
    bool found;
    size_t i;
    size_t index;

    XASSERT_NOT_NULL(drv);
    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(drv_data_list);

    pthread_mutex_lock(&drv->mutex);

    /* Update the active data list. */
    changed = false;
    for (i = 0; i < drv_data_list->len; ++i) {
        drv_data = &drv_data_list->data[i];
        index = get_active_data_index(drv, drv_data, &found);
        if (found) {
            data = &xv_A(drv->active_data, index);
            /*
             * We can assert this because ctx_alloc should have failed if the
             * periods did not match.
             */
            XASSERT_EQ(data->data->period_ns, drv_data->period_ns);
            ++data->refcount;
        }
        else {
            err = push_drv_data(drv, drv_data);
            if (err != HOUND_OK) {
                /*
                 * If this fails, we are out of memory, so the entire
                 * active_data list is invalid!
                 */
                goto out;
            }
            changed = true;
        }
    }

    /* Tell the driver to change what data it generates. */
    if (changed) {
        err = drv_op_setdata(drv, drv_data_list);
        if (err != HOUND_OK) {
            goto error_driver_setdata;
        }
    }

    /* Start the driver if needed, and tell the I/O layer what we need. */
    ++drv->refcount;
    if (drv->refcount == 1) {
        err = drv_op_start(drv, &drv->fd);
        if (err != HOUND_OK) {
            goto error_driver_start;
        }

        err = io_add_fd(drv->fd, drv);
        if (err != HOUND_OK) {
            goto error_io_add_fd;
        }
    }

    err = io_add_queue(drv->fd, queue);
    if (err != HOUND_OK) {
        goto error_io_add_queue;
    }

    err = HOUND_OK;
    goto out;

error_io_add_queue:
    io_remove_fd(drv->fd);
error_io_add_fd:
error_driver_start:
    --drv->refcount;
error_driver_setdata:
    for (i = 0; i < drv_data_list->len; ++i) {
        drv_data = &drv_data_list->data[i];
        index = get_active_data_index(drv, drv_data, &found);
        /* We previously added this data, so it should be found. */
        XASSERT(found);
        data = &xv_A(drv->active_data, index);
        --data->refcount;
        if (data->refcount == 0) {
            RM_VEC_INDEX(drv->active_data, index);
        }
    }
out:
    pthread_mutex_unlock(&drv->mutex);
    return err;
}

static
void cleanup_drv_data(
    struct driver *drv,
    const struct hound_data_rq_list *drv_data_list)
{
    struct data *data;
    struct hound_data_rq *drv_data;
    hound_err err;
    bool found;
    size_t i;
    size_t index;

    for (i = 0; i < drv_data_list->len; ++i) {
        drv_data = &drv_data_list->data[i];
        index = get_active_data_index(drv, drv_data, &found);
        if (found) {
            data = &xv_A(drv->active_data, index);
            ++data->refcount;
        }
        else {
            err = push_drv_data(drv, drv_data);
            if (err != HOUND_OK) {
                /* We have run out of memory! */
                return;
            }
        }
    }
}

hound_err driver_unref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *drv_data_list)
{
    bool changed;
    struct data *data;
    struct hound_data_rq *drv_data;
    hound_err err;
    bool found;
    size_t i;
    size_t index;

    XASSERT_NOT_NULL(drv);
    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(drv_data_list);

    pthread_mutex_lock(&drv->mutex);

    /* Update the active data list. */
    changed = false;
    for (i = 0; i < drv_data_list->len; ++i) {
        drv_data = &drv_data_list->data[i];
        index = get_active_data_index(drv, drv_data, &found);
        /*
         * We should never unref a driver unless it was already reffed, so our
         * data should be in this last.
         */
        XASSERT(found);
        data = &xv_A(drv->active_data, index);
        --data->refcount;
        if (data->refcount == 0) {
            RM_VEC_INDEX(drv->active_data, index);
            changed = true;
        }
    }

    /* Stop the driver if needed. */
    --drv->refcount;
    if (drv->refcount == 0) {
        io_remove_fd(drv->fd);

        err = drv_op_stop(drv);
        if (err != HOUND_OK) {
            goto error_driver_stop;
        }
        drv->fd = FD_INVALID;
    }
    else {
        /*
         * io_remove_fd destroys the driver queues, so we need to explicitly
         * remove ourselves only if the driver is still active.
         */
        io_remove_queue(drv->fd, queue);

        if (changed) {
            err = drv_op_setdata(drv, drv_data_list);
            if (err != HOUND_OK) {
                goto error_driver_setdata;
            }
        }
    }

    err = HOUND_OK;
    goto out;

error_driver_stop:
    ++drv->refcount;
error_driver_setdata:
    cleanup_drv_data(drv, drv_data_list);
out:
    pthread_mutex_unlock(&drv->mutex);
    return err;
}

hound_err driver_get(hound_data_id data_id, struct driver **drv)
{
    hound_err err;
    xhiter_t iter;

    pthread_rwlock_rdlock(&s_driver_rwlock);

    iter = xh_get(DATA_MAP, s_data_map, data_id);
    if (iter == xh_end(s_data_map)) {
        err = HOUND_DATA_ID_DOES_NOT_EXIST;
        goto out;
    }
    *drv = xh_val(s_data_map, iter);

    err = HOUND_OK;

out:
    pthread_rwlock_unlock(&s_driver_rwlock);
    return err;
}

bool driver_period_supported(
    struct driver *drv,
    hound_data_id id,
    hound_data_period period)
{
    const struct hound_datadesc *desc;
    bool found;
    size_t i;
    size_t j;

    XASSERT_NOT_NULL(drv);

    pthread_rwlock_rdlock(&s_driver_rwlock);

    for (i = 0; i < drv->datacount; ++i) {
        desc = &drv->data[i];
        if (desc->id == id) {
            break;
        }
    }
    if (i == drv->datacount) {
        found = false;
        goto out;
    }

    /* A driver specifying no periods means any period is permissable. */
    if (desc->period_count == 0) {
        found = true;
        goto out;
    }

    found = false;
    for (j = 0; j < desc->period_count; ++j) {
        if (desc->avail_periods[j] == period) {
            found = true;
            goto out;
        }
    }

out:
    pthread_rwlock_unlock(&s_driver_rwlock);
    return found;
}
