/**
 * @file      driver_ops.h
 * @brief     Header file for wrappers for driver operations, to ensure
 *            safe/correct handling of driver contexts. All driver operations
 *            should be called through these wrappers rather than directly.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2018 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_DRIVER_OPS_H_
#define HOUND_PRIVATE_DRIVER_OPS_H_

#include <hound_private/driver.h>
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
    pthread_mutex_t mutex;
    refcount_val refcount;

    char device_id[HOUND_DEVICE_ID_MAX];
    hound_data_count datacount;
    struct hound_datadesc *data;

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
        set_active_drv(drv); \
        return drv->ops.name(args); \
    }

#define DEFINE_DRV_OP(name, prototype, args) \
    _DEFINE_DRV_OP(name, TOKENIZE(const struct driver *drv, prototype), TOKENIZE(args))

#define DEFINE_DRV_OP_VOID(name) _DEFINE_DRV_OP(name, struct driver *drv,)

DEFINE_DRV_OP(init, void *data, data)
DEFINE_DRV_OP_VOID(destroy)
DEFINE_DRV_OP(reset, void *data, data)
DEFINE_DRV_OP(device_id, char *device_id, device_id)
DEFINE_DRV_OP(
    datadesc,
    TOKENIZE(struct hound_datadesc **desc, hound_data_count *count),
    TOKENIZE(desc, count))
DEFINE_DRV_OP(setdata, const struct hound_data_rq_list *data, data)
DEFINE_DRV_OP(
    parse,
    TOKENIZE(const uint8_t *buf, size_t *bytes, struct hound_record *record),
    TOKENIZE(buf, bytes, record))
DEFINE_DRV_OP(start, int *fd, fd)
DEFINE_DRV_OP(next, hound_data_id id, id)
DEFINE_DRV_OP(
    next_bytes,
    TOKENIZE(hound_data_id id, size_t bytes),
    TOKENIZE(id, bytes))
DEFINE_DRV_OP_VOID(stop)

#endif /* HOUND_PRIVATE_DRIVER_OPS_H_ */
