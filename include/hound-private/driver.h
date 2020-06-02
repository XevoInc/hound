/**
 * @file      driver.h
 * @brief     Private driver functionality shared by multiple compilation units.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_DRIVER_H_
#define HOUND_PRIVATE_DRIVER_H_

#include <hound/hound.h>
#include <hound-private/io.h>
#include <hound-private/queue.h>
#include <stdbool.h>

#define HOUND_DRIVER_REGISTER_PRIO 102
#define HOUND_DRIVER_REGISTER_FUNC __attribute__((constructor(HOUND_DRIVER_REGISTER_PRIO)))

struct schema_desc {
    hound_data_id data_id;
    const char *name;
    size_t fmt_count;
    struct hound_data_fmt *fmts;
};

struct drv_datadesc {
    /** True if this descriptor is enabled; false otherwise. */
    bool enabled;

    /** For enabled data, the number of periods in the avail_periods array. */
    hound_period_count period_count;

    /** For enabled data, the available data periods for this descriptor. */
    hound_data_period *avail_periods;

    /* The schema for this descriptor. */
    const struct schema_desc *schema_desc;
};

struct driver_ops {
    hound_err (*init)(
        const char *path,
        size_t arg_count,
        const struct hound_init_arg *args);

    hound_err (*destroy)(void);

    /**
     * Get the device ID for the backing device.
     *
     * @param device_name a pointer to a string with length HOUND_DEVICE_NAME_MAX,
     * including the '\0' character. The driver must fill this in with a device
     * ID and include the '\0' character. If the driver does not have or cannot
     * find a device ID, it should fill in an empty string.
     *
     * @return an error code
     */
    hound_err (*device_name)(char *device_name);

    /**
     * Get the data descriptors supported by this driver.
     *
     * @param desc_count the length of the descriptors array
     * @param descs a pointer to an array of data descriptors as parsed by the
     *              driver's schema. The driver should set the "enabled" member
     *              of the struct to true if the descriptor is available and
     *              false otherwise, and fill in the frequnecies at which
     *              enabled data is available.
     *
     * @return an error code
     */
    hound_err (*datadesc)(size_t desc_count, struct drv_datadesc *descs);

    /**
     * Sets the data the driver should be prepared to generate when start() is
     * called.
     *
     * @param data the data to be generated
     *
     * @return an error code
     */
    hound_err (*setdata)(const struct hound_data_rq_list *data);

    /**
     * Called when the driver's fd is ready to read or write data. A driver must
     * implement either poll or parse, but not both. If it implements poll, then
     * it is responsible for reading and writing to its polled fd and creating
     * records.
     *
     * @param events the events returned by the most recent call to poll()
     * @param next_events to be filled in with the next events that should be
     *                    monitored. For more details, see the manpage for
     *                    poll(). If this is not set, the value will be
     *                    unchanged from the last time poll() was called.
     * @param timeout_enabled set to true if poll should be called again after a
     *                        timeout even if no events have occurred. This will
     *                        directly become an argument into the poll
     *                        syscall in the Hound I/O core.
     * @param timeout if timeout_enabled is set to true, the number of
     *                nanoseconds to wait until calling poll again.
     */
    hound_err (*poll)(
        short events,
        short *next_events,
        bool *timeout_enabled,
        hound_data_period *timeout);

    /**
     * Parse the raw data from the I/O layer and produce one or more records. A
     * driver must implement either poll or parse, but not both. If it
     * implements parse, then the I/O core will read any data available on the
     * driver's fd and pass it into parse, as a buffer.
     *
     * @param buf the raw data coming from the I/O layer
     * @param bytes the number of bytes in the buffer
     *
     * @return an error code
     */
    hound_err (*parse)(unsigned char *buf, size_t bytes);

    /**
     * Start the driver producing data.
     *
     * @param fd to be filled in with an fd on which the Hound core will poll
     *
     * @return an error code
     */
    hound_err (*start)(int *fd);

    /**
     * Ask the driver to generate a value of the given ID. This will be called
     * only if the driver advertises data values with period 0 (on-demand data).
     *
     * @param id a data ID
     *
     * @return an error code
     */
    hound_err (*next)(hound_data_id id);

    /**
     * Stop the driver from producing data and frees resources associated with
     * the driver's open fd.
     *
     * @return an error code
     */
    hound_err (*stop)(void);
};

size_t get_type_size(hound_type type);

/**
 * A function that drivers should use for any allocations they need to do.
 *
 * @param bytes the number of bytes to allocate to the pointer
 *
 * @return a pointer to the allocated memory, or NULL if the allocation failed.
 */
void *drv_alloc(size_t bytes);

/**
 * A function that drivers should use for any reallocations they need to do.
 *
 * @param p a pointer to realloc
 * @param bytes the number of bytes to reallocate to the pointer
 *
 * @return a pointer to the allocated memory, or NULL if the allocation failed.
 */
void *drv_realloc(void *p, size_t bytes);

/**
 * Driver version of strdup.
 *
 * @param s a string to duplicate
 *
 * @return a new, duplicated string, or NULL if the duplication failed.
 */
char *drv_strdup(const char *s);

/**
 * Free a pointer allocated by drv_alloc.
 *
 * @param p a pointer to free
 */
void drv_free(void *p);

/**
 * Gets the currently set driver context.
 *
 * @return the currently set driver context pointer, or NULL if no context has
 * been set.
 */
void *drv_ctx(void);

/**
 * Set a driver context void * for private driver context.
 *
 * @return a driver context pointer
 */
void drv_set_ctx(void *ctx);

/**
 * Get the driver's associated file descriptor.
 *
 * @return the driver's file descriptor
 */
int drv_fd(void);

void driver_init_statics(void);
void driver_destroy_statics(void);

/** Forward declaration for use as opaque pointer. */
struct driver;

hound_err driver_get_dev_name(hound_dev_id id, const char **name);
bool driver_is_pull_mode(const struct driver *drv);
bool driver_is_push_mode(const struct driver *drv);

hound_err driver_get_datadescs(struct hound_datadesc **descs, size_t *len);
void driver_free_datadescs(struct hound_datadesc *descs);

void driver_register(const char *name, struct driver_ops *ops);

hound_err driver_init(
    const char *name,
    const char *path,
    const char *schema_base,
    const char *schema,
    size_t arg_count,
    const struct hound_init_arg *args);

hound_err driver_destroy(const char *path);
hound_err driver_destroy_all(void);

hound_err driver_next(struct driver *drv, hound_data_id id, size_t n);

/*
 * Take a reference on this driver, causing the driver to start if it's the
 * first reference.
 */
hound_err driver_ref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *data_rq_list,
    bool modify);

/*
 * Drop a reference on this driver, causing the driver to stop if it's the last
 * reference.
 */
hound_err driver_unref(
    struct driver *drv,
    struct queue *queue,
    const struct hound_data_rq_list *data_rq_list,
    bool modify);

hound_err driver_get(hound_data_id id, struct driver **drv);

bool driver_period_supported(
    struct driver *drv,
    hound_data_id id,
    hound_data_period period);

/* #defines so drivers don't have to peek into the I/O subsystem. */
#define drv_push_records io_push_records
#define drv_default_pull io_default_pull
#define drv_default_push io_default_push

#endif /* HOUND_PRIVATE_DRIVER_H_ */
