/**
 * @file      driver.c
 * @brief     Hound driver subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/driver-ops.h>
#include <hound-private/error.h>
#include <hound-private/io.h>
#include <hound-private/log.h>
#include <hound-private/parse/schema.h>
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

/* Forward declaration. */
hound_err driver_destroy_nolock(const char *path);

static pthread_rwlock_t s_driver_rwlock = PTHREAD_RWLOCK_INITIALIZER;

void driver_init_statics(void)
{
    s_data_map = xh_init(DATA_MAP);
    XASSERT_NOT_NULL(s_data_map);
    s_device_map = xh_init(DEVICE_MAP);
    XASSERT_NOT_NULL(s_device_map);
    s_ops_map = xh_init(OPS_MAP);
    XASSERT_NOT_NULL(s_ops_map);
}

void driver_destroy_statics(void)
{
    const char *path;

    xh_destroy(OPS_MAP, s_ops_map);
    xh_foreach_key(s_device_map, path,
        driver_destroy_nolock(path);
    );
    xh_destroy(DEVICE_MAP, s_device_map);
    xh_destroy(DATA_MAP, s_data_map);
}

PUBLIC_API
void driver_register(const char *name, struct driver_ops *ops)
{
    xhiter_t iter;
    int ret;

    XASSERT_NOT_NULL(ops);
    XASSERT_NOT_NULL(ops->init);
    XASSERT_NOT_NULL(ops->destroy);
    XASSERT_NOT_NULL(ops->device_name);
    XASSERT_NOT_NULL(ops->datadesc);
    XASSERT_NOT_NULL(ops->setdata);
    XASSERT_NOT_NULL(ops->start);
    XASSERT_NOT_NULL(ops->stop);
    XASSERT_NOT_NULL(ops->poll);

    /*
     * If we can't initialize a driver this early on (this is called from
     * library constructors), then something is severely messed up and we might
     * as well assert.
     */
    iter = xh_put(OPS_MAP, s_ops_map, name, &ret);
    XASSERT_NEQ(ret, -1);

    /*
     * Driver names are determined at compile-time, so we shouldn't try to
     * register the same driver name twice.
     */
    XASSERT_NEQ(ret, 0);

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

hound_err driver_get_datadescs(struct hound_datadesc **descs, size_t *len)
{
    struct driver *drv;
    hound_err err;
    const struct hound_datadesc *pos;
    size_t size;

    NULL_CHECK(len);
    NULL_CHECK(descs);

    pthread_rwlock_rdlock(&s_driver_rwlock);

    /* Find the required data size. */
    size = 0;
    xh_foreach_value(s_device_map, drv,
        size += drv->desc_count;
    );

    *len = size;
    if (size == 0) {
        /*
         * If there's no data, set descs to NULL to help protect caller's against
         * freeing random memory when they call hound_free_datadescs to cleanup.
         */
        *descs = NULL;
        err = HOUND_OK;
        goto out;
    }
    else {
        /* Allocate. */
        *descs = malloc(size*sizeof(**descs));
        if (*descs == NULL) {
            err = HOUND_OOM;
            goto out;
        }
    }

    pos = *descs;
    xh_foreach_value(s_device_map, drv,
        memcpy((void *) pos, drv->descs, drv->desc_count*sizeof(*drv->descs));
        pos += drv->desc_count;
    );

    err = HOUND_OK;

out:
    pthread_rwlock_unlock(&s_driver_rwlock);
    return err;
}

void driver_free_datadescs(struct hound_datadesc *descs)
{
    /*
     * Note: This frees the user-facing data descriptor but not the descriptor
     * allocated by the driver, which we store and free only when the driver
     * unregisters.
     */
    free(descs);
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

size_t get_type_size(hound_type type)
{
    switch (type) {
        case HOUND_TYPE_BOOL:
            return sizeof(bool);
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

static
void copy_desc(
    struct hound_datadesc *datadesc,
    struct drv_datadesc *drv_desc,
    hound_dev_id dev_id)
{
    struct schema_desc *schema_desc;

    schema_desc = (struct schema_desc *) drv_desc->schema_desc;
    datadesc->data_id = schema_desc->data_id;
    datadesc->dev_id = dev_id;
    datadesc->name = schema_desc->name;

    /*
     * We need to nullify the pointer members of the schema descriptor
     * because we are moving them into the driver data descriptor. When we
     * free the schema descriptors, we need to not do a double-free.
     *
     * If you like C++, pretend this is std::move :).
     */
    datadesc->period_count = drv_desc->period_count;
    datadesc->avail_periods = drv_desc->avail_periods;
    datadesc->fmt_count = schema_desc->fmt_count;
    datadesc->fmts = schema_desc->fmts;
    drv_desc->avail_periods = NULL;
    schema_desc->name = NULL;
    schema_desc->fmt_count = 0;
    schema_desc->fmts = NULL;
}

static
void destroy_drv_desc(struct drv_datadesc *desc)
{
    free(desc->avail_periods);
}

bool driver_is_pull_mode(const struct driver *drv)
{
    return drv->ops.poll == drv_default_pull;
}

bool driver_is_push_mode(const struct driver *drv)
{
    return drv->ops.poll == drv_default_push;
}

PUBLIC_API
hound_err driver_init(
    const char *name,
    const char *path,
    const char *schema_base,
    const char *schema,
    size_t arg_count,
    const struct hound_init_arg *args)
{
    size_t desc_count;
    struct driver *drv;
    struct drv_datadesc *drv_desc;
    struct drv_datadesc *drv_descs;
    char *drv_path;
    size_t enabled_count;
    hound_err err;
    struct hound_data_fmt *fmt;
    size_t i;
    size_t j;
    xhiter_t iter;
    size_t next_index;
    size_t offset;
    const struct driver_ops *ops;
    int ret;
    struct schema_desc *schema_desc;
    struct schema_desc *schema_descs;
    size_t size;

    NULL_CHECK(name);
    NULL_CHECK(path);

    iter = xh_get(OPS_MAP, s_ops_map, name);
    if (iter == xh_end(s_ops_map)) {
        return HOUND_DRIVER_NOT_REGISTERED;
    }
    ops = xh_val(s_ops_map, iter);

    if (strnlen(path, PATH_MAX) == PATH_MAX) {
        return HOUND_INVALID_STRING;
    }

    if (schema_base == NULL) {
        schema_base = CONFIG_HOUND_SCHEMADIR;
    }
    else if (strnlen(schema_base, PATH_MAX) == PATH_MAX) {
        return HOUND_INVALID_STRING;
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
    init_mutex(&drv->state_lock);
    init_mutex(&drv->op_lock);
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

    err = schema_parse(schema_base, schema, &desc_count, &schema_descs);
    if (err != HOUND_OK) {
        goto error_schema_parse;
    }

    /* Make sure the descriptors and formats are sane. */
    for (i = 0; i < desc_count; ++i) {
        schema_desc = &schema_descs[i];
        XASSERT_NOT_NULL(schema_desc->name);
        XASSERT_GTE(schema_desc->fmt_count, 1);
        XASSERT_NOT_NULL(schema_desc->fmts);

        offset = 0;
        for (j = 0; j < schema_desc->fmt_count; ++j) {
            fmt = &schema_desc->fmts[j];
            fmt->offset = offset;
            if (fmt->type == HOUND_TYPE_BYTES) {
                /*
                 * A variable-length type (bytes) must be the last specified format,
                 * or else the caller won't be able to parse its records.
                 */
                XASSERT_FALSE(fmt->size == 0 && j != schema_desc->fmt_count-1);
            }
            else {
                /*
                 * Fill in the size for non-byte types, where the size is
                 * implicit and not given in the schema.
                 */
                size = get_type_size(fmt->type);
                fmt->size = size;
                offset += size;
            }
        }
    }

    /*
     * Allocate driver descriptors for the schema data, which drivers will use
     * to fill in the missing information (enabled/disabled, available periods)
     * for each schema descriptor.
     */
    drv_descs = malloc(desc_count * sizeof(*drv_descs));
    if (drv_descs == NULL) {
        err = HOUND_OOM;
        goto error_alloc_drv_descs;
    }

    /* Initialize driver descriptors before passing them into the driver. */
    for (i = 0; i < desc_count; ++i) {
        drv_desc = &drv_descs[i];
        drv_desc->enabled = false;
        drv_desc->period_count = 0;
        drv_desc->avail_periods = NULL;
        drv_desc->schema_desc = &schema_descs[i];
    }

    /* Get all the supported data for the driver. */
    err = drv_op_datadesc(drv, desc_count, drv_descs);
    if (err != HOUND_OK) {
        goto error_drv_datadesc;
    }

    /*
     * Count the number of enabled descriptors so we can allocate a data
     * descriptor array. While we're looping through the descriptors, validate
     * that the driver gave us sane data.
     */
    enabled_count = 0;
    for (i = 0; i < desc_count; ++i) {
        drv_desc = &drv_descs[i];
        if (!drv_desc->enabled) {
            continue;
        }
        ++enabled_count;

        /* Verify that multiple drivers don't claim the same data ID. */
        iter = xh_get(DATA_MAP, s_data_map, drv_desc->schema_desc->data_id);
        if (iter != xh_end(s_data_map)) {
            err = HOUND_CONFLICTING_DRIVERS;
            goto error_drv_datadesc;
        }

    }
    if (enabled_count == 0) {
        err = HOUND_NO_DESCS_ENABLED;
        goto error_drv_datadesc;
    }

    /* Copy driver descriptors into user-facing data descriptors. */
    drv->descs = malloc(enabled_count * sizeof(*drv->descs));
    if (drv->descs == NULL) {
        err = HOUND_OOM;
        goto error_drv_datadesc;
    }

    drv->desc_count = enabled_count;
    next_index = 0;
    for (i = 0; i < desc_count; ++i) {
        if (!drv_descs[i].enabled) {
            continue;
        }
        copy_desc(&drv->descs[next_index], &drv_descs[i], drv->id);
        ++next_index;
    }

    for (i = 0; i < desc_count; ++i) {
        destroy_drv_desc(&drv_descs[i]);
        destroy_schema_desc(&schema_descs[i]);
    }
    free(drv_descs);
    free(schema_descs);

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

    for (i = 0; i < drv->desc_count; ++i ) {
        iter = xh_put(DATA_MAP, s_data_map, drv->descs[i].data_id, &ret);
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
    free(drv->descs);
error_drv_datadesc:
    for (i = 0; i < desc_count; ++i) {
        destroy_drv_desc(&drv_descs[i]);
    }
    free(drv_descs);
error_alloc_drv_descs:
    for (i = 0; i < desc_count; ++i) {
        destroy_schema_desc(&schema_descs[i]);
    }
    free(schema_descs);
error_schema_parse:
error_device_name:
    err = drv_op_destroy(drv);
    if (err != HOUND_OK) {
        hound_log_err(err, "driver %p failed to destroy", (void *) drv);
    }
error_init:
    free(drv);
out:
    pthread_rwlock_unlock(&s_driver_rwlock);
    return err;
}

static
void drv_destroy_desc(struct hound_datadesc *desc)
{
    drv_free((void *) desc->name);
    drv_free((void *) desc->avail_periods);
    destroy_desc_fmts(desc->fmt_count, desc->fmts);
}

static
hound_err driver_remove_from_maps(const char *path, struct driver **out_drv)
{
    struct driver *drv;
    const char *drv_path;
    struct driver *drv_iter;
    xhiter_t iter;

    NULL_CHECK(path);

    /* Make sure the driver is actually registered. */
    iter = xh_get(DEVICE_MAP, s_device_map, path);
    if (iter == xh_end(s_device_map)) {
        return HOUND_DRIVER_NOT_REGISTERED;
    }
    drv_path = xh_key(s_device_map, iter);
    drv = xh_val(s_device_map, iter);

    /* Make sure the driver is not in-use. */
    if (drv->refcount != 0) {
        return HOUND_DRIVER_IN_USE;
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

    *out_drv = drv;

    return HOUND_OK;
}

static
void driver_destroy_obj(struct driver *drv)
{
    hound_err err;
    size_t i;

    /* Finally, stop and free the driver. */
    err = drv_op_destroy(drv);
    if (err != HOUND_OK) {
        hound_log_err(err, "driver %p failed to destroy", (void *) drv);
    }

    /* Free the driver-allocated data descriptor. */
    for (i = 0; i < drv->desc_count; ++i) {
        drv_destroy_desc(&drv->descs[i]);
    }
    free(drv->descs);

    destroy_mutex(&drv->state_lock);
    destroy_mutex(&drv->op_lock);
    xv_destroy(drv->active_data);
    free(drv);
}

hound_err driver_destroy_nolock(const char *path)
{
    struct driver *drv;
    hound_err err;

    err = driver_remove_from_maps(path, &drv);
    if (err != HOUND_OK) {
        return err;
    }
    driver_destroy_obj(drv);

    return HOUND_OK;
}

hound_err driver_destroy(const char *path)
{
    struct driver *drv;
    hound_err err;

    pthread_rwlock_wrlock(&s_driver_rwlock);
    err = driver_remove_from_maps(path, &drv);
    pthread_rwlock_unlock(&s_driver_rwlock);
    if (err != HOUND_OK) {
        return err;
    }

    driver_destroy_obj(drv);

    return HOUND_OK;
}

hound_err driver_destroy_all(void)
{
    hound_err drv_err;
    hound_err err;
    const char *path;

    err = HOUND_OK;
    pthread_rwlock_wrlock(&s_driver_rwlock);
    xh_foreach_key(s_device_map, path,
        drv_err = driver_destroy_nolock(path);
        if (drv_err != HOUND_OK) {
            err = drv_err;
            hound_log_err(
                drv_err,
                "Failed to destroy driver at path %s",
                path);
        }
    );
    pthread_rwlock_unlock(&s_driver_rwlock);

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
        if (data->rq->id == drv_data->id &&
            data->rq->period_ns == drv_data->period_ns) {
            *found = true;
            return i;
        }
    }

    *found = false;
    return SIZE_MAX;
}

static
struct data *get_active_data(
    const struct driver *drv,
    const struct hound_data_rq *drv_data)
{
    struct data *active_data;
    bool found;
    size_t index;

    index = get_active_data_index(drv, drv_data, &found);
    if (found) {
        active_data = &xv_A(drv->active_data, index);
    }
    else {
        active_data = NULL;
    }

    return active_data;
}

static
hound_err push_drv_data(struct driver *drv, struct hound_data_rq *rq)
{
    struct data *data;

    data = xv_pushp(struct data, drv->active_data);
    if (data == NULL) {
        /* We are out of memory, so the entire active_data list is invalid! */
        hound_log_err(
            HOUND_OOM,
            "Failed to push drv data onto active data list",
            rq->id);
        return HOUND_OOM;
    }
    data->refcount = 1;
    data->rq = rq;

    return HOUND_OK;
}


hound_err driver_next(struct driver *drv, hound_data_id id, size_t n)
{
    hound_err err;
    size_t i;

    XASSERT_NOT_NULL(drv);

    lock_mutex(&drv->state_lock);
    for (i = 0; i < n; ++i) {
        err = drv_op_next(drv, id);
        if (err != HOUND_OK) {
            goto out;
        }
    }
    unlock_mutex(&drv->state_lock);
    err = HOUND_OK;

out:
    return err;
}

static
hound_err ref_data_list(
    struct driver *drv,
    const struct hound_data_rq_list *rq_list,
    bool *out_changed)
{
    bool changed;
    struct data *data;
    hound_err err;
    size_t i;
    struct hound_data_rq *rq;

    changed = false;
    for (i = 0; i < rq_list->len; ++i) {
        rq = &rq_list->data[i];
        data = get_active_data(drv, rq);
        if (data != NULL) {
            ++data->refcount;
        }
        else {
            err = push_drv_data(drv, rq);
            if (err != HOUND_OK) {
                /* We have run out of memory! */
                goto out;
            }
            changed = true;
        }
    }

    err = HOUND_OK;

out:
    if (out_changed != NULL) {
        *out_changed = changed;
    }

    return err;
}

static
bool unref_data_list(
    struct driver *drv,
    const struct hound_data_rq_list *rq_list)
{
    bool changed;
    struct data *data;
    bool found;
    size_t i;
    size_t index;
    struct hound_data_rq *rq;

    changed = false;
    for (i = 0; i < rq_list->len; ++i) {
        rq = &rq_list->data[i];
        index = get_active_data_index(drv, rq, &found);
        /* We previously added this data, so it should be found. */
        XASSERT(found);
        data = &xv_A(drv->active_data, index);
        --data->refcount;
        if (data->refcount == 0) {
            xv_quickdel(drv->active_data, index);
            changed = true;
        }
    }

    return changed;
}

hound_err driver_ref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *rq_list,
    bool modify)
{
    bool changed;
    hound_err err;
    hound_err err2;

    XASSERT_NOT_NULL(drv);
    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(rq_list);

    lock_mutex(&drv->state_lock);

    /* Update the active data list. */
    err = ref_data_list(drv, rq_list, &changed);
    if (err != HOUND_OK) {
        goto out;
    }

    /* Tell the driver to change what data it generates. */
    if (changed) {
        err = drv_op_setdata(drv, rq_list);
        if (err != HOUND_OK) {
            goto error_driver_setdata;
        }
    }

    /* If we're doing a modify operation, polling should already be paused. */
    if (!modify) {
        io_pause_poll();
    }

    /* Start the driver if needed, and tell the I/O layer what we need. */
    ++drv->refcount;
    if (drv->refcount == 1) {
        /*
         * If we are doing a modify operation, then we never stopped the driver,
         * so we shouldn't start it again.
         */
        if (!modify) {
            err = drv_op_start(drv, &drv->fd);
            if (err != HOUND_OK) {
                goto error_driver_start;
            }
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

    io_resume_poll();

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
    if (!modify) {
        io_resume_poll();
    }
error_driver_setdata:
    (void) unref_data_list(drv, rq_list);
out:
    unlock_mutex(&drv->state_lock);
    return err;
}

hound_err driver_unref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *rq_list,
    bool modify)
{
    bool changed;
    hound_err err;
    hound_err err2;

    XASSERT_NOT_NULL(drv);
    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(rq_list);

    lock_mutex(&drv->state_lock);

    /* Update the active data list. */
    changed = unref_data_list(drv, rq_list);

    io_pause_poll();

    /* Stop the driver if needed. */
    --drv->refcount;
    if (drv->refcount == 0) {
        io_remove_fd(drv->fd);

        /*
         * If we're doing a modify operation, don't stop the driver, as we would
         * lose its fd and thus lose data.
         */
        if (!modify) {
            err = drv_op_stop(drv);
            if (err != HOUND_OK) {
                err2 = io_add_fd(drv->fd, drv);
                if (err2 != HOUND_OK) {
                    hound_log_err(
                        err2,
                        "driver %p failed to add fd %d",
                        (void *) drv,
                        drv->fd);
                }
                goto error_driver_op;
            }
            drv->fd = FD_INVALID;
        }
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
                err2 = io_add_queue(drv->fd, rq_list, queue);
                hound_log_err(err2, "driver %p failed to queue", (void *) drv);
                goto error_driver_op;
            }
        }
    }

    /*
     * If we're doing a modify operation, don't resume polling, as we want to do
     * that only once the entire operation is finished. Without this safe-guard,
     * we will drop data between this call and the corresponding driver_ref
     * call, since data will continue to be generated but have no backing queue
     * associated with it.
     */
    if (!modify) {
        io_resume_poll();
    }

    err = HOUND_OK;
    goto out;

error_driver_op:
    ++drv->refcount;
    io_resume_poll();
    err2 = ref_data_list(drv, rq_list, NULL);
    if (err2 != HOUND_OK) {
        hound_log_err(err2, "driver %p failed to ref data list", (void *) drv);
    }
out:
    unlock_mutex(&drv->state_lock);
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

    for (i = 0; i < drv->desc_count; ++i) {
        desc = &drv->descs[i];
        if (desc->data_id == id) {
            break;
        }
    }
    if (i == drv->desc_count) {
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
