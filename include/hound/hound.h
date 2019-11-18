/**
 * @file      hound.h
 * @brief     Hound public library header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_H_
#define HOUND_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Errors. */

/** Hound error codes are negative while errno codes are positive. */
typedef enum {
    HOUND_OK = 0,
    HOUND_NULL_VAL = -1,
    HOUND_OOM = -2,
    HOUND_DRIVER_ALREADY_REGISTERED = -3,
    HOUND_DRIVER_NOT_REGISTERED = -4,
    HOUND_DRIVER_IN_USE = -5,
    HOUND_MISSING_DEVICE_IDS = -6,
    HOUND_CONFLICTING_DRIVERS = -7,
    HOUND_NO_DATA_REQUESTED = -8,
    HOUND_DATA_ID_DOES_NOT_EXIST = -9,
    HOUND_CTX_ACTIVE = -10,
    HOUND_CTX_NOT_ACTIVE = -11,
    HOUND_EMPTY_QUEUE = -12,
    HOUND_MISSING_CALLBACK = -13,
    HOUND_PERIOD_UNSUPPORTED = -14,
    HOUND_IO_ERROR = -15,
    HOUND_QUEUE_TOO_SMALL = -16,
    HOUND_INVALID_STRING = -17,
    HOUND_DRIVER_UNSUPPORTED = -18,
    HOUND_DRIVER_FAIL = -19,
    HOUND_INVALID_VAL = -20,
    HOUND_INTR = -21,
    HOUND_DEV_DOES_NOT_EXIST = -22,
    HOUND_TOO_MUCH_DATA_REQUESTED = -23,
    HOUND_DUPLICATE_DATA_REQUESTED = -24
} hound_err;

/** Returns a human-readable error string. The string must not be modified or
 * freed, but it may be modified by subsequent calls to hound_strerror or the
 * libc strerror class of functions. */
const char *hound_strerror(hound_err err);

/*
 * Current list of datatypes for fixed-function devices (those that always
 * generate the same types of data and thus can declare their data types in
 * code).
 */
#define HOUND_DATA_CAN ((hound_data_id) 0x00000000)
#define HOUND_DATA_GPS ((hound_data_id) 0x00000001)
#define HOUND_DATA_ACCEL ((hound_data_id) 0x00000002)
#define HOUND_DATA_GYRO ((hound_data_id) 0x00000003)

/* Data. */

typedef uint_least32_t hound_data_id;
typedef uint_least8_t hound_dev_id;
typedef uint_least64_t hound_seqno;
typedef uint_least32_t hound_record_size;

/** Max length for a device name, including the null character. */
#define HOUND_DEVICE_NAME_MAX 32

/** Max number of data IDs requested per context. */
#define HOUND_MAX_DATA_REQ 1000

struct hound_record {
    hound_seqno seqno;
    hound_data_id data_id;
    hound_dev_id dev_id;
    struct timespec timestamp;
    hound_record_size size;
    uint8_t *data;
};

typedef void (*hound_cb)(const struct hound_record *rec, void *cb_ctx);
typedef uint_fast8_t hound_period_count;
typedef uint_fast64_t hound_data_period;

/*
 * These are SI units as much as possible. Time is an exception, as specifying
 * everything in seconds will result in floating point issues.
 */
typedef enum {
    HOUND_UNIT_DEGREE,
    HOUND_UNIT_KELVIN,
    HOUND_UNIT_KG_PER_S,
    HOUND_UNIT_METER,
    HOUND_UNIT_METERS_PER_S,
    HOUND_UNIT_METERS_PER_S_SQUARED,
    HOUND_UNIT_NONE,
    HOUND_UNIT_PASCAL,
    HOUND_UNIT_PERCENT,
    HOUND_UNIT_RAD,
    HOUND_UNIT_RAD_PER_S,
    HOUND_UNIT_NANOSECOND
} hound_unit;

typedef enum {
    HOUND_TYPE_FLOAT,
    HOUND_TYPE_DOUBLE,
    HOUND_TYPE_INT8,
    HOUND_TYPE_UINT8,
    HOUND_TYPE_INT16,
    HOUND_TYPE_UINT16,
    HOUND_TYPE_INT32,
    HOUND_TYPE_UINT32,
    HOUND_TYPE_INT64,
    HOUND_TYPE_UINT64,
    HOUND_TYPE_BYTES
} hound_type;

struct hound_data_fmt {
    const char *name;
    const char *desc;
    hound_unit unit;
    size_t offset;
    /* Length 0 means "all of the data." */
    size_t len;
    hound_type type;
};

struct hound_datadesc {
    hound_data_id data_id;
    hound_dev_id dev_id;
    const char *name;
    hound_period_count period_count;
    const hound_data_period *avail_periods;
    size_t fmt_count;
    struct hound_data_fmt *fmts;
};

struct hound_data_rq {
    hound_data_id id;
    hound_data_period period_ns;
};

struct hound_data_rq_list {
    size_t len;
    struct hound_data_rq *data;
};

struct hound_rq {
    size_t queue_len;
    hound_cb cb;
    void *cb_ctx;
    struct hound_data_rq_list rq_list;
};

hound_err hound_get_datadesc(struct hound_datadesc **desc, size_t *len);
void hound_free_datadesc(struct hound_datadesc *desc);

/* Devices. */

/** Opaque pointer to an I/O context. */
struct hound_ctx;

hound_err hound_get_dev_name(hound_dev_id id, const char **name);

hound_err hound_alloc_ctx(struct hound_ctx **ctx, const struct hound_rq *rq);
hound_err hound_free_ctx(struct hound_ctx *ctx);

/**
 * Starts the context generating and queueing data.
 *
 * @param ctx a context
 *
 * @return an error code
 */
hound_err hound_start(struct hound_ctx *ctx);

/**
 * Stops the generation and queueing of data.
 *
 * @param ctx a context
 *
 * @return an error code
 */
hound_err hound_stop(struct hound_ctx *ctx);

/**
 * Asks all drivers underlying this context to produce data. This call is useful
 * only for pull mode (non-periodic, period == 0) data, and it does nothing for
 * periodic drivers.
 *
 * @param ctx a context
 * @param n the number of records to produce.
 *
 * @return an error code
 */
hound_err hound_next(struct hound_ctx *ctx, size_t n);

/**
 * Triggers callback invocations to process queued data, blocking until all
 * requested data has finished a callback invocation.  Also calls hound_next()
 * for any pull-mode data.
 *
 * @param ctx a context
 * @param records the number of records to read. Reads the records as fast as
 *                possible, blocking when the queue is empty until a new item
 *                is available.
 *
 * @return an error code
 */
hound_err hound_read(struct hound_ctx *ctx, size_t records);

/**
 * Triggers callback invocations to process queued data. If fewer than n records
 * are available, processes callbacks on what is available instead of blocking
 * until n records are available. Callbacks for any available data are still
 * guaranteed to have completed upon return. This function does not call
 * hound_next(), so it is useless for pull-mode data.
 *
 * @param ctx a context
 * @param records trigger callbacks for up to the specified records. If fewer
 *                than this count is available, reads all available records.
 * @param read filled in to indicate how many records were actually read.
 *
 * @return an error code
 */
hound_err hound_read_nowait(struct hound_ctx *ctx, size_t records, size_t *read);

/**
 * Triggers callback invocations to process queued data. If fewer than n records
 * are available, processes callbacks on what is available instead of blocking
 * until n records are available. Callbacks for any available data are still
 * guaranteed to have completed upon return. This function does not call
 * hound_next(), so it is useless for pull-mode data.
 *
 * This form of read_nowait will trigger callbacks on up to the specified number
 * of bytes of records. The callback will take the same form as usual,
 * triggering on a per-record basis. However, the sum of the record sizes
 * triggered will not exceed the specified number of bytes.
 *
 * @param ctx a context
 * @param bytes trigger callbacks for up to the specified bytes of records. If
 *              fewer than this count is available, reads all available records.
 * @param records_read filled in to indicate how many records were actually read.
 * @param read filled in to indicate how many bytes were actually read.
 *
 * @return an error code
 */
hound_err hound_read_bytes_nowait(
    struct hound_ctx *ctx,
    size_t bytes,
    size_t *records_read,
    size_t *bytes_read);

/**
 * Triggers callback invocations to process all currently available data. This
 * is equivalent to calling hound_read_nowait(ctx, INFINITY).
 *
 * @param ctx a context
 * @param read filled in to indicate how many records were actually read.
 *
 * @return an error code
 */
hound_err hound_read_all_nowait(struct hound_ctx *ctx, size_t *read);

/**
 * Returns how many records are currently available in the queue.
 *
 * @param ctx a context
 * @param count filled in with the current number of available records.
 *
 * @return an error code
 */
hound_err hound_queue_length(struct hound_ctx *ctx, size_t *count);

/**
 * Returns the maximum queue length.
 *
 * @param ctx a context
 * @param count filled in with the max number of records.
 *
 * @return an error code
 */
hound_err hound_max_queue_length(struct hound_ctx *ctx, size_t *count);

/**
 * Unregisters the driver at the given path, effectively unloading the backing
 * driver.
 *
 * @param path the path to a device file
 *
 * @return an error code
 */
hound_err hound_unregister_driver(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* HOUND_H_ */
