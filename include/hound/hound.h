/**
 * @file      hound.h
 * @brief     Hound public library header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_H_
#define HOUND_H_

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef uint_fast64_t hound_data;

/* Errors. */
typedef int hound_err;

/** Hound error codes are negative while errno codes are positive. */
enum hound_err_t {
    HOUND_OK = 0,
    HOUND_NULL_VAL = -1,
    HOUND_NAME_NOT_FOUND = -2,
    HOUND_OOM = -3,
    HOUND_DRIVER_ALREADY_REGISTERED = -4,
    HOUND_DRIVER_NOT_REGISTERED = -5,
    HOUND_DRIVER_IN_USE = -6,
    HOUND_MISSING_DEVICE_IDS = -7,
    HOUND_CONFLICTING_DRIVERS = -8,
    HOUND_NO_DATA_REQUESTED = -9,
    HOUND_DATA_ID_DOES_NOT_EXIST = -10,
    HOUND_CTX_ALREADY_ACTIVE = -11,
    HOUND_CTX_NOT_ACTIVE = -12,
    HOUND_EMPTY_QUEUE = -13,
    HOUND_MISSING_CALLBACK = -14,
    HOUND_FREQUENCY_UNSUPPORTED = -15,
    HOUND_FREQUENCY_CONFLICTING = -16,
    HOUND_IO_ERROR = -17,
    HOUND_QUEUE_TOO_SMALL = -18
};

/** Returns a human-readable error string. The string must not be modified or
 * freed. */
const char *hound_perror(hound_err e);

/* Data. */
enum hound_datatype {
  HOUND_DEVICE_TEMPERATURE = 0,
  HOUND_DEVICE_ORIENTATION = 1,
  HOUND_DEVICE_PRESSURE = 2,
  HOUND_DEVICE_HYGROMETER = 3,
  HOUND_DEVICE_LIGHT = 4,
  HOUND_DEVICE_ACCELEROMETER = 5,
  HOUND_DEVICE_MAGNEMOMETER = 6,
  HOUND_DEVICE_GYROSCOPE = 7,
  HOUND_DEVICE_MAX = 8
};

#define HOUND_OBDII_BASE (0xffffffffff000000u)
#define HOUND_OBDII_ID(mode, pid) (HOUND_OBDII_BASE + (mode << 16) + pid)

/* Data. */

typedef uint_least64_t hound_data_id;
typedef uint_least64_t hound_seqno;
typedef uint_least32_t hound_record_size;

struct hound_record {
    hound_data_id id;
    hound_seqno seqno;
    struct timespec timestamp;
    hound_record_size size;
    uint8_t *data;
};

typedef void (*hound_cb)(struct hound_record *rec);
typedef uint_fast64_t hound_data_freq;

struct hound_datadesc {
    hound_data_id id;
    const char *name;
    hound_data_freq freq;
};

struct hound_data_rq {
    hound_data_id id;
    hound_data_freq freq;
};

struct hound_data_rq_list {
    size_t len;
    struct hound_data_rq *data;
};

struct hound_rq {
    size_t queue_len;
    hound_cb cb;
    struct hound_data_rq_list rq_list;
};

/* Devices. */

/** Opaque pointer to an I/O context. */
struct hound_ctx;

hound_err hound_get_datadesc(const struct hound_datadesc ***desc, size_t *len);
void hound_free_datadesc(const struct hound_datadesc **desc);

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
 * Triggers callback invocations to process queued data.
 *
 * @param ctx a context
 * @param n the number of records to read. Reads the records as fast as
 *              possible, blocking when the queue is empty until a new item is
 *              available.
 *
 * @return an error code
 */
hound_err hound_read(struct hound_ctx *ctx, size_t n);

/**
 * Triggers callback invocations to process queued data without blocking.
 *
 * @param ctx a context
 * @param n trigger callbacks for up to count records. If fewer than n records
 *          are available, reads all available records.
 * @param read filled in to indicate how many records were actually read.
 *
 * @return an error code
 */
hound_err hound_read_async(struct hound_ctx *ctx, size_t n, size_t *read);

/**
 * Triggers callback invocations to process all currently available data.
 *
 * @param ctx a context
 * @param read filled in to indicate how many records were actually read.
 *
 * @return an error code
 */
hound_err hound_read_all(struct hound_ctx *ctx, size_t *read);

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

#endif /* HOUND_H_ */
