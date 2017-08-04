/**
 * @file      driver.c
 * @brief     Hound driver subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/assert.h>
#include <hound/driver.h>
#include <hound/log.h>
#include <hound/hound.h>
#include <hound_private/driver.h>
#include <hound_private/io.h>
#include <hound_private/klib/khash.h>
#include <hound_private/klib/kvec.h>
#include <hound_private/util.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

/* TODO: make logging consistent everywhere */

#define FD_INVALID (-1)

struct data {
    refcount_val refcount;
    struct hound_drv_data *data;
};

struct driver {
    pthread_mutex_t mutex;
    refcount_val refcount;

    hound_datacount datacount;
    const struct hound_drv_datadesc *data;

    kvec_t(struct data) active_data;

    hound_device_id_count device_id_count;
    const char **device_ids;

    int fd;
    struct driver_ops ops;
};

/* device path --> driver ID */
KHASH_MAP_INIT_STR(DEVICE_MAP, struct driver *)
static khash_t(DEVICE_MAP) *s_device_map = NULL;

/* data ID --> data info */
KHASH_MAP_INIT_INT64(DATA_MAP, struct driver *)
static khash_t(DATA_MAP) *s_data_map;

static pthread_rwlock_t s_driver_rwlock = PTHREAD_RWLOCK_INITIALIZER;

void driver_init(void)
{
    s_data_map = kh_init(DATA_MAP);
    s_device_map = kh_init(DEVICE_MAP);
}

void driver_destroy(void)
{
    kh_destroy(DEVICE_MAP, s_device_map);
    kh_destroy(DATA_MAP, s_data_map);
}

hound_err driver_get_datadesc(const struct hound_datadesc ***desc, size_t *len)
{
    struct driver *drv;
    hound_err err;
    const struct hound_datadesc **pos;
    size_t size;



    NULL_CHECK(len);
    NULL_CHECK(desc);

    pthread_rwlock_rdlock(&s_driver_rwlock);

    /* Find the required data size. */
    size = 0;
    kh_foreach_value(s_data_map, drv,
        size += drv->datacount;
    );

    *len = size;
    if (size == 0) {
        /*
         * If there's no data, set desc to NULL to help protect caller's against
         * freeing random memory when they call hound_free_datadesc to cleanup.
         */
        *desc = NULL;
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
    kh_foreach_value(s_data_map, drv,
        memcpy((void *) pos, drv->data, drv->datacount*sizeof(drv->data));
        pos += drv->datacount;
    );

    err = HOUND_OK;

out:
    pthread_rwlock_unlock(&s_driver_rwlock);
    return err;
}

void driver_free_datadesc(const struct hound_datadesc **desc)
{
    free(desc);
}

hound_err driver_register(
    const char *path,
    const struct hound_io_driver *driver)
{
    struct driver *drv;
    hound_err err;
    size_t i;
    khiter_t iter;
    int ret;

    NULL_CHECK(path);

    pthread_rwlock_wrlock(&s_driver_rwlock);

    iter = kh_get(DEVICE_MAP, s_device_map, path);
    if (iter != kh_end(s_device_map)) {
        err = HOUND_DRIVER_ALREADY_REGISTERED;
        goto out;
    }

    NULL_CHECK(driver);
    NULL_CHECK(driver->init);
    NULL_CHECK(driver->destroy);
    NULL_CHECK(driver->reset);
    NULL_CHECK(driver->device_ids);
    NULL_CHECK(driver->datadesc);
    NULL_CHECK(driver->setdata);
    NULL_CHECK(driver->parse);
    NULL_CHECK(driver->start);
    NULL_CHECK(driver->stop);

    /* Init. */
    err = driver->init(malloc);
    if (err != HOUND_OK) {
        goto out;
    }

    /* Allocate. */
    drv = malloc(sizeof(*drv));
    if (drv == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    /* Device IDs. */
    err = driver->device_ids(&drv->device_ids, &drv->device_id_count);
    if (err != HOUND_OK) {
        goto error_device_ids;
    }

    for (i = 0; i < drv->device_id_count; ++i) {
        if (drv->device_ids[i] == NULL) {
            err = HOUND_MISSING_DEVICE_IDS;
            goto error_missing_ids;
        }
    }

    /* Get all the supported data for the driver. */
    err = driver->datadesc(&drv->data, &drv->datacount);
    if (err != HOUND_OK) {
        goto error_datadesc;
    }

    /* Set the rest of the driver fields. */
    pthread_mutex_init(&drv->mutex, NULL);
    drv->refcount = 0;
    drv->fd = FD_INVALID;
    kv_init(drv->active_data);
    drv->ops.destroy = driver->destroy;
    drv->ops.reset = driver->reset;
    drv->ops.setdata = driver->setdata;
    drv->ops.parse = driver->parse;
    drv->ops.start = driver->start;
    drv->ops.stop = driver->stop;

    /* Verify that all descriptors are sane. */
    for (i = 0; i < drv->datacount; ++i) {
        iter = kh_get(DATA_MAP, s_data_map, drv->data[i].id);
        if (iter != kh_end(s_data_map)) {
            err = HOUND_CONFLICTING_DRIVERS;
            goto error_conflicting_drivers;
        }
    }

    /*
     * Finally, commit the driver into all the maps.
     */
    iter = kh_put(DEVICE_MAP, s_device_map, path, &ret);
    if (ret == -1) {
        err = HOUND_OOM;
        goto error_device_map_put;
    }
    kh_val(s_device_map, iter) = drv;

    for (i = 0; i < drv->datacount; ++i ) {
        iter = kh_put(DATA_MAP, s_data_map, drv->data[i].id, &ret);
        if (ret == -1) {
            err = HOUND_OOM;
            goto error_data_map_put;
        }
        kh_val(s_data_map, iter) = drv;
    }

    err = HOUND_OK;
    goto out;

error_data_map_put:
    iter = kh_get(DEVICE_MAP, s_device_map, path);
    HOUND_ASSERT_NEQ(iter, kh_end(s_device_map));
    kh_del(DEVICE_MAP, s_device_map, iter);
error_device_map_put:
error_conflicting_drivers:
error_datadesc:
error_missing_ids:
error_device_ids:
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
    khiter_t iter;

    NULL_CHECK(path);

    pthread_rwlock_wrlock(&s_driver_rwlock);

    /* Make sure the driver is actually registered. */
    iter = kh_get(DEVICE_MAP, s_device_map, path);
    if (iter == kh_end(s_device_map)) {
        err = HOUND_DRIVER_NOT_REGISTERED;
        goto out;
    }
    drv = kh_val(s_device_map, iter);

    /* Make sure the driver is not in-use. */
    if (drv->refcount != 0) {
        err = HOUND_DRIVER_IN_USE;
        goto out;
    }

    /* Remove the driver from all the maps so no one can access it. */
    kh_foreach_value_iter(s_device_map, drv_iter, iter,
        if (drv_iter == drv) {
            kh_del(DEVICE_MAP, s_device_map, iter);
        }
    );

    /* Remove the driver from the data map for each datatype it manages. */
    kh_foreach_value_iter(s_data_map, drv_iter, iter,
        if (drv_iter == drv) {
            kh_del(DATA_MAP, s_data_map, iter);
        }
    );

    pthread_rwlock_unlock(&s_driver_rwlock);

    /* Finally, stop and free the driver. */
    err = drv->ops.destroy();
    if (err != HOUND_OK) {
        hound_log_err(err, "driver %p failed to destroy", (void *) drv);
    }

    err = pthread_mutex_destroy(&drv->mutex);
    HOUND_ASSERT_EQ(err, 0);
    kv_destroy(drv->active_data);
    free(drv);

    err = HOUND_OK;

out:
    return err;
}

static
size_t get_active_data_index(
    const struct driver *drv,
    const struct hound_drv_data *drv_data,
    bool *found)
{
    const struct data *data;
    size_t i;

    for (i = 0; i < kv_size(drv->active_data); ++i) {
        data = &kv_A(drv->active_data, i);
        if (data->data->id == drv_data->id) {
            *found = true;
            return i;
        }
    }

    *found = false;
    return SIZE_MAX;
}

static
void push_drv_data(struct driver *drv, struct hound_drv_data *drv_data)
{
    struct data *data;

    data = kv_pushp(struct data, drv->active_data);
    if (kv_data(drv->active_data) == NULL) {
        hound_log_err(
            HOUND_OOM,
            "Failed to push drv data onto active data list",
            drv_data->id);
        goto error;
    }
    data->refcount = 1;
    data->data = drv_data;

    return;

error:
    free(data);
}

hound_err driver_ref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_drv_data_list *drv_data_list)
{
    bool changed;
    struct data *data;
    struct hound_drv_data *drv_data;
    hound_err err;
    bool found;
    size_t i;
    size_t index;

    HOUND_ASSERT_NOT_NULL(drv);
    HOUND_ASSERT_NOT_NULL(queue);
    HOUND_ASSERT_NOT_NULL(drv_data_list);

    pthread_mutex_lock(&drv->mutex);

    /* Update the active data list. */
    changed = false;
    for (i = 0; i < drv_data_list->len; ++i) {
        drv_data = &drv_data_list->data[i];
        index = get_active_data_index(drv, drv_data, &found);
        if (found) {
            data = &kv_A(drv->active_data, index);
            if (data->data->freq != drv_data->freq) {
                err = HOUND_FREQUENCY_CONFLICTING;
                goto error_data_loop;
            }
            else {
                ++data->refcount;
            }
        }
        else {
            push_drv_data(drv, drv_data);
            changed = true;
        }
    }

    /* Tell the driver to change what data it generates. */
    if (changed) {
        err = drv->ops.setdata(drv_data_list);
        if (err != HOUND_OK) {
            goto error_driver_setdata;
        }
    }

    /* Start the driver if needed, and tell the I/O layer what we need. */
    ++drv->refcount;
    if (drv->refcount == 1) {
        err = drv->ops.start(&drv->fd);
        if (err != HOUND_OK) {
            goto error_driver_start;
        }

        err = io_add_fd(drv->fd, &drv->ops);
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
    /*
     * Technically i will be this already, but if someone in the future reuses
     * i, it will break the cleanup code, so set this defensively.
     */
    i = drv_data_list->len;
error_data_loop:
    for (; i < drv_data_list->len; --i) {
        drv_data = &drv_data_list->data[i];
        index = get_active_data_index(drv, drv_data, &found);
        /* We previously added this data, so it should be found. */
        HOUND_ASSERT_TRUE(found);
        data = &kv_A(drv->active_data, index);
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
    const struct hound_drv_data_list *drv_data_list)
{
    struct data *data;
    struct hound_drv_data *drv_data;
    bool found;
    size_t i;
    size_t index;

    for (i = 0; i < drv_data_list->len; ++i) {
        drv_data = &drv_data_list->data[i];
        index = get_active_data_index(drv, drv_data, &found);
        if (found) {
            data = &kv_A(drv->active_data, index);
            ++data->refcount;
        }
        else {
            push_drv_data(drv, drv_data);
        }
    }
}

hound_err driver_unref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_drv_data_list *drv_data_list)
{
    bool changed;
    struct data *data;
    struct hound_drv_data *drv_data;
    hound_err err;
    bool found;
    size_t i;
    size_t index;

    HOUND_ASSERT_NOT_NULL(drv);
    HOUND_ASSERT_NOT_NULL(queue);
    HOUND_ASSERT_NOT_NULL(drv_data_list);

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
        HOUND_ASSERT_TRUE(found);
        data = &kv_A(drv->active_data, index);
        --data->refcount;
        if (data->refcount == 0) {
            RM_VEC_INDEX(drv->active_data, index);
            changed = true;
        }
    }

    /* Tell the driver to change what data it generates. */
    if (changed) {
        err = drv->ops.setdata(drv_data_list);
        if (err != HOUND_OK) {
            goto error_driver_setdata;
        }
    }

    /* Remove ourselves from the I/O layer. */
    io_remove_queue(drv->fd, queue);

    /* Stop the driver if needed. */
    --drv->refcount;
    if (drv->refcount == 0) {
        io_remove_fd(drv->fd);

        err = drv->ops.stop();
        if (err != HOUND_OK) {
            goto error_driver_stop;
        }
        drv->fd = FD_INVALID;
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
    khiter_t iter;

    pthread_rwlock_rdlock(&s_driver_rwlock);

    iter = kh_get(DATA_MAP, s_data_map, data_id);
    if (iter == kh_end(s_data_map)) {
        err = HOUND_DATA_ID_DOES_NOT_EXIST;
        goto out;
    }
    *drv = kh_val(s_data_map, iter);

    err = HOUND_OK;

out:
    pthread_rwlock_unlock(&s_driver_rwlock);
    return err;
}

bool driver_freq_supported(
    struct driver *drv,
    hound_data_id id,
    hound_data_freq freq)
{
    const struct hound_drv_datadesc *desc;
    bool found;
    size_t i;
    size_t j;

    HOUND_ASSERT_NOT_NULL(drv);

    pthread_rwlock_rdlock(&s_driver_rwlock);

    for (i = 0; i < drv->datacount; ++i) {
        desc = &drv->data[i];
        if (desc->id != id) {
            continue;
        }

        found = false;
        for (j = 0; j < desc->freq_count; ++j) {
            if (desc->avail_freq[j] == freq) {
                found = true;
                break;
            }
        }

        pthread_rwlock_unlock(&s_driver_rwlock);
        return found;
    }

    /*
     * This function should never be called unless the driver already supports
     * this data ID.
     */
    HOUND_ASSERT_ERROR;
}
