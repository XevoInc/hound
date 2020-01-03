/**
 * @file      driver.c
 * @brief     Hound driver subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _GNU_SOURCE
#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/driver-ops.h>
#include <hound-private/driver/util.h>
#include <hound-private/error.h>
#include <hound-private/io.h>
#include <hound-private/log.h>
#include <hound-private/schema.h>
#include <hound-private/util.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xlib/xhash.h>
#include <xlib/xvec.h>

#include "config.h"

#define FD_INVALID (-1)

/* driver name --> driver ops */
XHASH_MAP_INIT_STR(OPS_MAP, const struct driver_ops *)
static xhash_t(OPS_MAP) *s_ops_map = NULL;

/* device path --> driver */
XHASH_MAP_INIT_STR(DEVICE_MAP, struct driver *)
static xhash_t(DEVICE_MAP) *s_device_map = NULL;

/* data ID --> driver */
XHASH_MAP_INIT_INT64(DATA_MAP, struct driver *)
static xhash_t(DATA_MAP) *s_data_map;

static pthread_rwlock_t s_driver_rwlock = PTHREAD_RWLOCK_INITIALIZER;

void driver_init_statics(void)
{
    s_data_map = xh_init(DATA_MAP);
    XASSERT_NOT_NULL(s_data_map);
    s_device_map = xh_init(DEVICE_MAP);
    XASSERT_NOT_NULL(s_device_map);
    s_ops_map = xh_init(OPS_MAP);
    XASSERT_NOT_NULL(s_ops_map);

    driver_ops_init();
}

void driver_destroy_statics(void)
{
    const char *path;

    driver_ops_destroy();
    xh_destroy(OPS_MAP, s_ops_map);

    xh_foreach_key(s_device_map, path,
        driver_destroy(path);
    );
    xh_destroy(DEVICE_MAP, s_device_map);

    xh_destroy(DATA_MAP, s_data_map);
}

PUBLIC_API
void driver_register(const char *name, const struct driver_ops *ops)
{
    xhiter_t iter;
    int ret;

    XASSERT_NOT_NULL(ops);
    XASSERT_NOT_NULL(ops->init);
    XASSERT_NOT_NULL(ops->destroy);
    XASSERT_NOT_NULL(ops->device_name);
    XASSERT_NOT_NULL(ops->datadesc);
    XASSERT_NOT_NULL(ops->setdata);
    XASSERT_NOT_NULL(ops->parse);
    XASSERT_NOT_NULL(ops->start);
    XASSERT_NOT_NULL(ops->next);
    XASSERT_NOT_NULL(ops->stop);

    /*
     * If we can't initialize a driver this early on (this is called from
     * library constructors), then something is severely messed up and we might
     * as well assert.
     */
    iter = xh_put(OPS_MAP, s_ops_map, name, &ret);
    XASSERT_NEQ(ret, -1);
    xh_val(s_ops_map, iter) = ops;
    xh_trim(OPS_MAP, s_ops_map);
}

hound_err driver_get_dev_name(hound_dev_id id, const char **name)
{
    const struct driver *drv;
    hound_err err;

    pthread_rwlock_rdlock(&s_driver_rwlock);

    err = HOUND_DEV_DOES_NOT_EXIST;
    xh_foreach_value(s_device_map, drv,
        if (id == drv->id) {
            if (name != NULL) {
                *name = drv->device_name;
            }
            err = HOUND_OK;
            break;
        }
    );

    pthread_rwlock_unlock(&s_driver_rwlock);

    return err;
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

static hound_dev_id s_next_dev_id = 0;
static
hound_dev_id next_dev_id(void)
{
    hound_dev_id id;

    /* Must be called with the driver lock held. */
    id = s_next_dev_id;
    ++s_next_dev_id;
    return id;
}

static
size_t get_type_len(hound_type type)
{
    switch (type) {
        case HOUND_TYPE_FLOAT:
            return sizeof(float);
        case HOUND_TYPE_DOUBLE:
            return sizeof(double);
        case HOUND_TYPE_INT8:
            return sizeof(int8_t);
        case HOUND_TYPE_UINT8:
            return sizeof(uint8_t);
        case HOUND_TYPE_INT16:
            return sizeof(int16_t);
        case HOUND_TYPE_UINT16:
            return sizeof(uint16_t);
        case HOUND_TYPE_INT32:
            return sizeof(int32_t);
        case HOUND_TYPE_UINT32:
            return sizeof(uint32_t);
        case HOUND_TYPE_INT64:
            return sizeof(int64_t);
        case HOUND_TYPE_UINT64:
            return sizeof(uint64_t);
        case HOUND_TYPE_BYTES:
            return 0;
    }

    XASSERT_ERROR;
}

PUBLIC_API
hound_err driver_init(
    const char *name,
    const char *path,
    const char *schema_base,
    size_t arg_count,
    const struct hound_init_arg *args)
{
    struct hound_datadesc *desc;
    struct driver *drv;
    char *drv_path;
    hound_err err;
    struct hound_data_fmt *fmt;
    bool found;
    size_t i;
    size_t j;
    xhiter_t iter;
    size_t len;
    size_t offset;
    const struct driver_ops *ops;
    int ret;
    char schema[PATH_MAX];
    size_t schema_desc_count;
    struct schema_desc *schema_desc;
    struct schema_desc *schema_descs;

    NULL_CHECK(name);
    NULL_CHECK(path);

    iter = xh_get(OPS_MAP, s_ops_map, name);
    if (iter == xh_end(s_ops_map)) {
        return HOUND_DRIVER_NOT_REGISTERED;
    }
    ops = xh_val(s_ops_map, iter);

    if (strnlen(path, PATH_MAX) == PATH_MAX) {
        err = HOUND_INVALID_STRING;
        goto out;
    }

    if (schema_base == NULL) {
        schema_base = CONFIG_HOUND_SCHEMADIR;
    }
    else if (strnlen(schema_base, PATH_MAX) == PATH_MAX) {
        err = HOUND_INVALID_STRING;
        goto out;
    }

    pthread_rwlock_wrlock(&s_driver_rwlock);

    iter = xh_get(DEVICE_MAP, s_device_map, path);
    if (iter != xh_end(s_device_map)) {
        err = HOUND_DRIVER_ALREADY_PRESENT;
        goto out;
    }

    /* Allocate. */
    drv = malloc(sizeof(*drv));
    if (drv == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    /* Initialize driver fields. */
    pthread_mutex_init(&drv->state_lock, NULL);
    pthread_mutex_init(&drv->op_lock, NULL);
    drv->refcount = 0;
    drv->fd = FD_INVALID;
    xv_init(drv->active_data);
    drv->ops = *ops;
    drv->id = next_dev_id();
    drv->ctx = NULL;

    /* Init. */
    err = drv_op_init(drv, path, arg_count, args);
    if (err != HOUND_OK) {
        goto error_init;
    }

    /* Device IDs. */
    err = drv_op_device_name(drv, drv->device_name);
    if (err != HOUND_OK) {
        goto error_device_name;
    }

    if (strnlen(drv->device_name, HOUND_DEVICE_NAME_MAX)
        == HOUND_DEVICE_NAME_MAX) {
        err = HOUND_INVALID_STRING;
        goto error_device_name;
    }

    /* Get all the supported data for the driver. */
    err = drv_op_datadesc(
        drv,
        &drv->datacount,
        &drv->data,
        schema,
        &drv->sched_mode);
    if (err != HOUND_OK) {
        goto error_datadesc;
    }

    /*
     * Parse and verify the schema, and calculate offsets from the provided
     * lengths.
     */
    if (strnlen(schema, PATH_MAX) == PATH_MAX) {
        err = HOUND_INVALID_STRING;
        goto error_datadesc;
    }

    err = schema_parse(schema_base, schema, &schema_desc_count, &schema_descs);
    if (err != HOUND_OK) {
        goto error_datadesc;
    }

    /* Make sure the descriptors and formats are sane. */
    for (i = 0; i < schema_desc_count; ++i) {
        schema_desc = &schema_descs[i];
        XASSERT_NOT_NULL(schema_desc->name);
        XASSERT_GTE(schema_desc->fmt_count, 1);
        XASSERT_NOT_NULL(schema_desc->fmts);

        offset = 0;
        for (j = 0; j < schema_desc->fmt_count; ++j) {
            fmt = &schema_desc->fmts[j];
            /*
             * A variable-length type (bytes) must be the last specified format,
             * or else the caller won't be able to parse its records.
             */
            XASSERT_FALSE(fmt->type == HOUND_TYPE_BYTES &&
                          j != schema_desc->fmt_count-1);
            len = get_type_len(fmt->type);
            fmt->len = len;
            fmt->offset = offset;
            offset += len;
        }
    }

    /* Verify that all driver descriptors are sane. */
    for (i = 0; i < drv->datacount; ++i) {
        desc = &drv->data[i];
        if (desc->period_count > 0 && desc->avail_periods == NULL) {
            err = HOUND_NULL_VAL;
            goto error_datadesc;
        }

        /* Make sure the driver provides at most one entry for each data ID. */
        for (j = i+1; j < drv->datacount; ++j) {
            if (desc->data_id == drv->data[j].data_id) {
                err = HOUND_DESC_DUPLICATE;
            goto error_datadesc;
            }
        }

        /*
         * Make sure this data ID is mentioned in the schema, and copy its
         * format data.
         */
        found = false;
        for (j = 0; j < schema_desc_count; ++j) {
            schema_desc = &schema_descs[j];
            if (desc->data_id == schema_desc->data_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            err = HOUND_ID_NOT_IN_SCHEMA;
            goto error_datadesc;
        }
        /*
         * We need to nullify the pointer members of the schema descriptor
         * because we are moving them into the driver data descriptor. When we
         * free the schema descriptors, we need to not do a double-free.
         *
         * If you like C++, pretend this is std::move :).
         */
        desc->name = schema_desc->name;
        desc->fmt_count = schema_desc->fmt_count;
        desc->fmts = schema_desc->fmts;
        schema_desc->name = NULL;
        schema_desc->fmt_count = 0;
        schema_desc->fmts = NULL;

        iter = xh_get(DATA_MAP, s_data_map, drv->data[i].data_id);
        if (iter != xh_end(s_data_map)) {
            err = HOUND_CONFLICTING_DRIVERS;
            goto error_datadesc;
        }

        desc->dev_id = drv->id;
    }

    for (i = 0; i < schema_desc_count; ++i) {
        schema_desc = &schema_descs[i];
        destroy_schema_desc(schema_desc);
    }
    drv_free(schema_descs);

    /*
     * Finally, commit the driver into all the maps.
     */
    drv_path = strdup(path);
    if (drv_path == NULL) {
        err = HOUND_OOM;
        goto error_alloc_drv_path;
    }

    iter = xh_put(DEVICE_MAP, s_device_map, drv_path, &ret);
    if (ret == -1) {
        err = HOUND_OOM;
        goto error_device_map_put;
    }
    xh_val(s_device_map, iter) = drv;

    for (i = 0; i < drv->datacount; ++i ) {
        iter = xh_put(DATA_MAP, s_data_map, drv->data[i].data_id, &ret);
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
    free(drv_path);
error_alloc_drv_path:
error_datadesc:
error_device_name:
error_init:
    free(drv);
out:
    pthread_rwlock_unlock(&s_driver_rwlock);
    return err;
}

hound_err driver_destroy(const char *path)
{
    struct driver *drv;
    const char *drv_path;
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
    drv_path = xh_key(s_device_map, iter);
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

    free((char *) drv_path);

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

    err = pthread_mutex_destroy(&drv->state_lock);
    XASSERT_EQ(err, 0);
    err = pthread_mutex_destroy(&drv->op_lock);
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
        if (data->data->id == drv_data->id &&
            data->data->period_ns == drv_data->period_ns) {
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
    if (data == NULL) {
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

    pthread_mutex_lock(&drv->state_lock);
    for (i = 0; i < n; ++i) {
        err = drv_op_next(drv, id);
        if (err != HOUND_OK) {
            goto out;
        }
    }
    pthread_mutex_unlock(&drv->state_lock);
    err = HOUND_OK;

out:
    return err;
}

hound_err driver_ref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *rq_list)
{
    bool changed;
    struct data *data;
    struct hound_data_rq *rq;
    hound_err err;
    hound_err err2;
    bool found;
    size_t i;
    size_t index;

    XASSERT_NOT_NULL(drv);
    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(rq_list);

    pthread_mutex_lock(&drv->state_lock);

    /* Update the active data list. */
    changed = false;
    for (i = 0; i < rq_list->len; ++i) {
        rq = &rq_list->data[i];
        index = get_active_data_index(drv, rq, &found);
        if (found) {
            data = &xv_A(drv->active_data, index);
            ++data->refcount;
        }
        else {
            err = push_drv_data(drv, rq);
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
        err = drv_op_setdata(drv, rq_list);
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

    err = io_add_queue(drv->fd, rq_list, queue);
    if (err != HOUND_OK) {
        goto error_io_add_queue;
    }

    err = HOUND_OK;
    goto out;

error_io_add_queue:
    if (drv->refcount == 1) {
        io_remove_fd(drv->fd);
    }
error_io_add_fd:
    if (drv->refcount == 1) {
        err2 = drv_op_stop(drv);
        if (err2 != HOUND_OK) {
            hound_log_err(err2, "driver %p failed to stop", (void *) drv);
        }
    }
error_driver_start:
    --drv->refcount;
error_driver_setdata:
    for (i = 0; i < rq_list->len; ++i) {
        rq = &rq_list->data[i];
        index = get_active_data_index(drv, rq, &found);
        /* We previously added this data, so it should be found. */
        XASSERT(found);
        data = &xv_A(drv->active_data, index);
        --data->refcount;
        if (data->refcount == 0) {
            RM_VEC_INDEX(drv->active_data, index);
        }
    }
out:
    pthread_mutex_unlock(&drv->state_lock);
    return err;
}

static
void cleanup_drv_data(
    struct driver *drv,
    const struct hound_data_rq_list *rq_list)
{
    struct data *data;
    struct hound_data_rq *rq;
    hound_err err;
    bool found;
    size_t i;
    size_t index;

    for (i = 0; i < rq_list->len; ++i) {
        rq = &rq_list->data[i];
        index = get_active_data_index(drv, rq, &found);
        if (found) {
            data = &xv_A(drv->active_data, index);
            ++data->refcount;
        }
        else {
            err = push_drv_data(drv, rq);
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
    const struct hound_data_rq_list *rq_list)
{
    bool changed;
    struct data *data;
    hound_err err;
    bool found;
    size_t i;
    size_t index;
    struct hound_data_rq *rq;

    XASSERT_NOT_NULL(drv);
    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(rq_list);

    pthread_mutex_lock(&drv->state_lock);

    /* Update the active data list. */
    changed = false;
    for (i = 0; i < rq_list->len; ++i) {
        rq = &rq_list->data[i];
        index = get_active_data_index(drv, rq, &found);
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
        io_remove_queue(drv->fd, rq_list, queue);

        if (changed) {
            err = drv_op_setdata(drv, rq_list);
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
    cleanup_drv_data(drv, rq_list);
out:
    pthread_mutex_unlock(&drv->state_lock);
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

drv_sched_mode driver_get_sched_mode(const struct driver *drv)
{
    drv_sched_mode mode;

    pthread_rwlock_rdlock(&s_driver_rwlock);
    mode = drv->sched_mode;
    pthread_rwlock_unlock(&s_driver_rwlock);

    return mode;
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
        if (desc->data_id == id) {
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
