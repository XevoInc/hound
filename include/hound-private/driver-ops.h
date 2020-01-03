/**
 * @file      driver-ops.h
 * @brief     Header file for wrappers for driver operations, to ensure
 *            safe/correct handling of driver contexts. All driver operations
 *            should be called through these wrappers rather than directly.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_DRIVER_OPS_H_
#define HOUND_PRIVATE_DRIVER_OPS_H_

#include <hound-private/driver.h>
#include <pthread.h>
#include <xlib/xvec.h>

/**
 * Gets the active driver.
 *
 * @return the active driver
 */
void *get_active_drv(void);

/**
 * Sets the active driver.
 * @param drv a driver struct
 */
void set_active_drv(const struct driver *drv);

/**
 * Initialize driver ops resources.
 */
void driver_ops_init(void);

/**
 * Destroy driver ops resources.
 */
void driver_ops_destroy(void);

struct data {
    refcount_val refcount;
    struct hound_data_rq *data;
};

struct driver {
    pthread_mutex_t state_lock;
    pthread_mutex_t op_lock;
    refcount_val refcount;

    hound_dev_id id;
    char device_name[HOUND_DEVICE_NAME_MAX];
    size_t datacount;
    struct hound_datadesc *data;
    drv_sched_mode sched_mode;

    xvec_t(struct data) active_data;

    int fd;
    struct driver_ops ops;
    void *ctx;
};

#define TOKENIZE(...) __VA_ARGS__

#define _DEFINE_DRV_OP(name, prototype, args) \
    static inline \
    hound_err drv_op_##name(prototype) \
    { \
        hound_err err; \
        \
        set_active_drv(drv); \
        pthread_mutex_lock(&drv->op_lock); \
        err = drv->ops.name(args); \
        pthread_mutex_unlock(&drv->op_lock); \
        return err; \
    }

#define DEFINE_DRV_OP(name, prototype, args) \
    _DEFINE_DRV_OP(name, TOKENIZE(struct driver *drv, prototype), TOKENIZE(args))

#define DEFINE_DRV_OP_VOID(name) _DEFINE_DRV_OP(name, struct driver *drv,)

DEFINE_DRV_OP(
    init,
    TOKENIZE(
        const char *path,
        size_t arg_count,
        const struct hound_init_val *args),
    TOKENIZE(path, arg_count, args))
DEFINE_DRV_OP_VOID(destroy)
DEFINE_DRV_OP(device_name, char *device_name, device_name)
DEFINE_DRV_OP(
    datadesc,
    TOKENIZE(
        size_t *desc_count,
        struct hound_datadesc **descs,
        drv_sched_mode *mode),
    TOKENIZE(desc_count, descs, mode))
DEFINE_DRV_OP(setdata, const struct hound_data_rq_list *data, data)
DEFINE_DRV_OP(
    parse,
    TOKENIZE(
        uint8_t *buf,
        size_t *bytes,
        struct hound_record *records,
        size_t *record_count),
    TOKENIZE(buf, bytes, records, record_count))
DEFINE_DRV_OP(start, int *fd, fd)
DEFINE_DRV_OP(next, hound_data_id id, id)
DEFINE_DRV_OP(
    next_bytes,
    TOKENIZE(hound_data_id id, size_t bytes),
    TOKENIZE(id, bytes))
DEFINE_DRV_OP_VOID(stop)

#endif /* HOUND_PRIVATE_DRIVER_OPS_H_ */
