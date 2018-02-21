/**
 * @file      driver.h
 * @brief     Private driver functionality shared by multiple compilation units.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_DRIVER_H_
#define HOUND_PRIVATE_DRIVER_H_

#include <hound/hound.h>
#include <hound_private/queue.h>
#include <stdbool.h>

/** Max length for a device name, including '\0'. */
#define HOUND_DEVICE_NAME_MAX (32)
/** Max length for a device ID, including '\0'. */
#define HOUND_DEVICE_ID_MAX (32)

typedef uint_least8_t hound_data_count;
typedef uint_least8_t hound_period_count;

struct driver_ops {
    hound_err (*init)(void *data);

    hound_err (*destroy)(void);
    hound_err (*reset)(void *data);

    /**
     * Get the device ID for the backing device.
     *
     * @param device_id a pointer to a string with length HOUND_DEVICE_ID_MAX,
     * including the '\0' character. The driver must fill this in with a device
     * ID and include the '\0' character. If the driver does not have or cannot
     * find a device ID, it should fill in an empty string of just '0'.
     *
     * @return an error code
     */
    hound_err (*device_id)(char *device_id);

    /**
     * Get the data descriptors supported by this driver. These descriptors must
     * be allocated with drv_alloc. Their memory is owned by the driver core.
     *
     * @param desc a pointer to an array of data descriptors. The memory for
     *             for this array is owned by the driver and must not be
     *             modified.
     * @param count the length of the array.
     *
     * @return an error code
     */
    hound_err (*datadesc)(
            struct hound_datadesc **desc,
            hound_data_count *count);

    hound_err (*setdata)(const struct hound_data_rq_list *data);

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
     *               allocated using drv_alloc. The allocated memory is owned by
     *               the driver core.
     *
     * @return an error code
     */
    hound_err (*parse)(
        const uint8_t *buf,
        size_t *bytes,
        struct hound_record *record);

    hound_err (*start)(int *fd);
    hound_err (*next)(hound_data_id id);
    hound_err (*next_bytes)(hound_data_id id, size_t bytes);
    hound_err (*stop)(void);
};

/**
 * A function that drivers should use for any allocations they need to do.
 *
 * @return a pointer to the allocated memory, or NULL if the allocation failed.
 */
void *drv_alloc(size_t bytes);
/**
 * Free a pointer allocated by drv_alloc.
 *
 * @param p a pointer to free
 */
void drv_free(void *p);

void driver_init(void);
void driver_destroy(void);

/** Forward declaration for use as opaque pointer. */
struct driver;

hound_err driver_get_datadesc(struct hound_datadesc **desc, size_t *len);
void driver_free_datadesc(struct hound_datadesc *desc);

/**
 * Registers a new driver at the given path. A copy of the driver description is
 * made, so the memory ownership is not transferred.
 *
 * @param path the path to a device file
 * @param driver a driver description
 * @param data driver-specific initialization data
 *
 * @return an error code
 */
hound_err driver_register(
    const char *path,
    struct driver_ops *ops,
    void *data);
hound_err driver_unregister(const char *path);

hound_err driver_next(struct driver *drv, hound_data_id id, size_t n);

hound_err driver_ref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *data_rq_list);
hound_err driver_unref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *data_rq_list);

hound_err driver_get(hound_data_id id, struct driver **drv);

bool driver_period_supported(
    struct driver *drv,
    hound_data_id id,
    hound_data_period period);

#endif /* HOUND_PRIVATE_DRIVER_H_ */
