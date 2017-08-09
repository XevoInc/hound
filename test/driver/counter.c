/**
 * @file      counter.c
 * @brief     Counter driver implementation. This driver simply increments a
 *            counter every time it is read from in order to do a basic test of
 *            the I/O subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _GNU_SOURCE
#include <hound/error.h>
#include <hound/hound.h>
#include <hound/driver.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define FD_INVALID (-1)
#define UNUSED __attribute__((unused))

#define READ_END (0)
#define WRITE_END (1)

static const char *s_device_ids[] = {"counter"};
static const hound_data_period s_period = 0;
static const struct hound_drv_datadesc s_datadesc[] = {
    {
        .id = HOUND_DEVICE_TEMPERATURE,
        .name = "increasing-temperature-counter",
        .period_count = 1,
        .avail_periods = &s_period
    }
};

static hound_alloc *s_alloc;
int s_pipe[2] = { FD_INVALID, FD_INVALID };
static size_t s_count;
static uint8_t s_buf[sizeof(s_count)];
static size_t s_buf_bytes;

hound_err counter_init(hound_alloc alloc, void *data)
{
    if (data == NULL) {
        return HOUND_NULL_VAL;
    }
    s_count = *((__typeof__(s_count) *) data);
    s_alloc = alloc;
    s_buf_bytes = 0;

    return HOUND_OK;
}

hound_err counter_destroy(void)
{
    return HOUND_OK;
}

hound_err counter_reset(hound_alloc alloc, void *data)
{
    counter_destroy();
    counter_init(alloc, data);

    return HOUND_OK;
}

hound_err counter_device_ids(
    const char ***device_ids,
    hound_device_id_count *count)
{
    *count = ARRAYLEN(s_device_ids);
    *device_ids = s_device_ids;

    return HOUND_OK;
}

hound_err counter_datadesc(
    const struct hound_drv_datadesc **desc,
    hound_data_count *count)
{
    *count = ARRAYLEN(s_datadesc);
    *desc = s_datadesc;

    return HOUND_OK;
}

hound_err counter_setdata(UNUSED const struct hound_drv_data_list *data)
{
    return HOUND_OK;
}

hound_err counter_parse(
    const uint8_t *buf,
    size_t *bytes,
    struct hound_record *record)
{
    size_t consumed_bytes;
    hound_err err;

    HOUND_ASSERT_NOT_NULL(buf);
    HOUND_ASSERT_NOT_NULL(bytes);
    HOUND_ASSERT_GT(*bytes, 0);
    HOUND_ASSERT_NOT_NULL(record);

    if (*bytes + s_buf_bytes < sizeof(s_count)) {
        /* We have less than a full record. */
        memcpy(s_buf+s_buf_bytes, buf, *bytes);
        s_buf_bytes = *bytes;
        return HOUND_OK;
    }
    else {
        /* We have at least a full record. */
        err = clock_gettime(CLOCK_MONOTONIC, &record->timestamp);
        HOUND_ASSERT_EQ(err, 0);
        record->data = (*s_alloc)(sizeof(s_count));
        if (record->data == NULL) {
            return HOUND_OOM;
        }
        record->id = HOUND_DEVICE_TEMPERATURE;
        record->size = sizeof(s_count);
        consumed_bytes = sizeof(s_count) - s_buf_bytes;
        memcpy(record->data, s_buf, s_buf_bytes);
        memcpy(
            record->data+s_buf_bytes,
            buf+s_buf_bytes,
            consumed_bytes);

        s_buf_bytes = 0;
        *bytes -= consumed_bytes;
    }

    return HOUND_OK;
}

hound_err counter_start(int *fd)
{
    hound_err err;

    HOUND_ASSERT_EQ(s_pipe[READ_END], FD_INVALID);
    HOUND_ASSERT_EQ(s_pipe[WRITE_END], FD_INVALID);

    err = pipe(s_pipe);
    if (err != 0) {
        return err;
    }
    *fd = s_pipe[READ_END];

    return HOUND_OK;
}

hound_err counter_stop(void)
{
    hound_err err;

    HOUND_ASSERT_NEQ(s_pipe[READ_END], FD_INVALID);
    HOUND_ASSERT_NEQ(s_pipe[WRITE_END], FD_INVALID);

    err = close(s_pipe[READ_END]);
    HOUND_ASSERT_EQ(err, 0);
    err = close(s_pipe[WRITE_END]);
    HOUND_ASSERT_EQ(err, 0);

    s_pipe[READ_END] = FD_INVALID;
    s_pipe[WRITE_END] = FD_INVALID;

    return HOUND_OK;
}

void counter_next(void)
{
    size_t written;

    written = write(s_pipe[WRITE_END], &s_count, sizeof(s_count));
    HOUND_ASSERT_EQ(written, sizeof(s_count));

    ++s_count;
}

void counter_zero(void)
{
    s_count = 0;
}

struct hound_io_driver counter_driver = {
    .init = counter_init,
    .destroy = counter_destroy,
    .reset = counter_reset,
    .device_ids = counter_device_ids,
    .datadesc = counter_datadesc,
    .setdata = counter_setdata,
    .parse = counter_parse,
    .start = counter_start,
    .stop = counter_stop
};
