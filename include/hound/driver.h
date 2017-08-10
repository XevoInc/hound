/**
 * @file      driver.h
 * @brief     Hound library header for drivers to use.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_DRIVER_H_
#define HOUND_DRIVER_H_

#include <hound/hound.h>
#include <hound_private/klib/khash.h>

#define HOUND_DEVICE_ID_MAX_LEN (33)

typedef uint_least8_t hound_data_count;
typedef uint_least8_t hound_device_id_count;
typedef uint_least8_t hound_period_count;

struct hound_drv_datadesc {
    hound_data_id id;
    const char *name;
    hound_period_count period_count;
    const hound_data_period *avail_periods;
};

struct hound_drv_data {
    hound_data_id id;
    hound_data_period period_ns;
};

struct hound_drv_data_list {
    hound_data_count len;
    struct hound_drv_data *data;
};

typedef void *(hound_alloc)(size_t);

struct hound_io_driver {
    hound_err (*init)(hound_alloc alloc, void *data);

    hound_err (*destroy)(void);
    hound_err (*reset)(hound_alloc alloc, void *data);

    /**
     * Get the device IDs advertised by this driver.
     *
     * @param device_ids a pointer to an array of data descriptors. The memory
     *                   for this array is owned by the driver and must not be
     *                   modified.
     * @param count the length of the array.
     *
     * @return an error code
     */
    hound_err (*device_ids)(
            const char ***device_ids,
            hound_device_id_count *count);

    /**
     * Get the data descriptors supported by this driver.
     *
     * @param desc a pointer to an array of data descriptors. The memory for
     *             for this array is owned by the driver and must not be
     *             modified.
     * @param count the length of the array.
     *
     * @return an error code
     */
    hound_err (*datadesc)(
            const struct hound_drv_datadesc **desc,
            hound_data_count *count);

    hound_err (*setdata)(const struct hound_drv_data_list *data);

    /**
     * Parse the raw data from the I/O layer and produce a record.
     *
     * @param data the raw data coming from the I/O layer
     * @param bytes a pointer to the number of bytes in the buffer. The pointer
     *              should be filled in to indicate how many bytes are still
     *              left unconsumed by the driver. For example, if *bytes is 10
     *              and the driver consumes 8 bytes, the driver should set
     *              *bytes to 2 before returning. If the driver does not change
     *              the value of bytes (no bytes were consumed), then it is
     *              assumed that the driver has no more records to produce at
     *              this time, so parse will not be called again until new data
     *              is available. Note that the next time parse is called, the
     *              unconsumed data will *not* be in the buffer, so if the
     *              driver needs to reference the unconsumed bytes, it must
     *              store them itself.
     * @param record a record to be filled in. The record's data should be
     *               allocated by the driver using the allocator function passed
     *               to the driver at initialization time, and the allocated
     *               memory is owned by the driver core.
     *
     * @return an error code
     */
    hound_err (*parse)(
        const uint8_t *buf,
        size_t *bytes,
        struct hound_record *record);

    hound_err (*start)(int *fd);
    hound_err (*next)(hound_data_id id);
    hound_err (*stop)(void);
};

/**
 * Registers a new driver at the given path. A copy of the driver description is
 * made, so the memory ownership is not transferred.
 *
 * @param path the path to a device file
 * @param driver a driver description
 *
 * @return an error code
 */
hound_err hound_register_io_driver(
    const char *path,
    const struct hound_io_driver *driver,
    void *data);

/**
 * Unregisters the driver at the given path, effectively unloading the backing
 * driver.
 *
 * @param path the path to a device file
 *
 * @return an error code
 */
hound_err hound_unregister_io_driver(const char *path);

#endif /* HOUND_DRIVER_H_ */
