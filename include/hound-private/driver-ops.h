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
struct driver *get_active_drv(void);

/**
 * Sets the active driver.
 * @param drv a driver struct
 */
void set_active_drv(struct driver *drv);

/**
 * Clears the active driver. Must be called after set_active_drv when done using
 * the driver.
 */
void clear_active_drv(void);

struct data {
    refcount_val refcount;
    struct hound_data_rq rq;
};

XVEC_DEFINE(active_data_vec, struct data);

struct driver {
    pthread_mutex_t state_lock;
    pthread_mutex_t op_lock;
    refcount_val refcount;

    hound_dev_id id;
    char device_name[HOUND_DEVICE_NAME_MAX];
    size_t desc_count;
    struct hound_datadesc *descs;

    active_data_vec active_data;

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
        lock_mutex(&drv->op_lock); \
        XASSERT_NOT_NULL(drv->ops.name); \
        err = drv->ops.name(args); \
        unlock_mutex(&drv->op_lock); \
        clear_active_drv(); \
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
        const struct hound_init_arg *args),
    TOKENIZE(path, arg_count, args))
DEFINE_DRV_OP_VOID(destroy)
DEFINE_DRV_OP(device_name, char *device_name, device_name)
DEFINE_DRV_OP(
    datadesc,
    TOKENIZE(
        size_t desc_count,
        struct drv_datadesc *descs),
    TOKENIZE(desc_count, descs))
DEFINE_DRV_OP(setdata,
    TOKENIZE(
        const struct hound_data_rq *rqs,
        size_t rqs_len
    ),
    TOKENIZE(rqs, rqs_len))
DEFINE_DRV_OP(
    poll,
    TOKENIZE(
        short events,
        short *next_events,
        hound_data_period poll_time,
        bool *timeout_enabled,
        hound_data_period *timeout),
    TOKENIZE(events, next_events, poll_time, timeout_enabled, timeout))
DEFINE_DRV_OP(
    parse,
    TOKENIZE(unsigned char *buf, size_t bytes),
    TOKENIZE(buf, bytes))
DEFINE_DRV_OP(start, int *fd, fd)
DEFINE_DRV_OP(next, hound_data_id id, id)
DEFINE_DRV_OP_VOID(stop)

#endif /* HOUND_PRIVATE_DRIVER_OPS_H_ */
