/**
 * @file      driver.h
 * @brief     Private driver functionality shared by multiple compilation units.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_DRIVER_H_
#define HOUND_PRIVATE_DRIVER_H_

#include <hound/driver.h>
#include <hound/hound.h>
#include <hound_private/queue.h>
#include <stdbool.h>

typedef uint_least8_t drv_datacount;

struct driver_ops {
    hound_err (*destroy)(void);
    hound_err (*reset)(hound_alloc alloc, void *data);
    hound_err (*setdata)(const struct hound_drv_data_list *data);
    hound_err (*parse)(
        const uint8_t *buf,
        size_t *bytes,
        struct hound_record *record);
    hound_err (*start)(int *fd);
    hound_err (*stop)(void);
};

void driver_init(void);
void driver_destroy(void);

hound_err driver_get_datadesc(const struct hound_datadesc ***desc, size_t *len);
void driver_free_datadesc(const struct hound_datadesc **desc);

hound_err driver_register(
    const char *path,
    const struct hound_io_driver *driver,
    void *data);

hound_err driver_unregister(const char *path);

/** Opaque pointer for the private driver API. */
struct driver;

hound_err driver_ref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_drv_data_list *drv_data_list);
hound_err driver_unref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_drv_data_list *drv_data_list);

hound_err driver_get(hound_data_id data_id, struct driver **drv);

bool driver_period_supported(
    struct driver *drv,
    hound_data_id id,
    hound_data_period period);

#endif /* HOUND_PRIVATE_DRIVER_H_ */
