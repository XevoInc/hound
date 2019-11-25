/**
 * @file      counter.c
 * @brief     Counter driver implementation. This driver simply increments a
 *            counter every time it is read from in order to do a basic test of
 *            the I/O subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _GNU_SOURCE
#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/driver/util.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <hound-test/id.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FD_INVALID (-1)

#define READ_END (0)
#define WRITE_END (1)

static const char *s_device_name = "counter";
static const hound_data_period s_period = 0;
static const struct hound_datadesc s_datadesc = {
    .data_id = HOUND_DATA_COUNTER,
    .period_count = 1,
    .avail_periods = &s_period
};

int s_pipe[2] = { FD_INVALID, FD_INVALID };
static uint64_t s_count;

static
hound_err counter_init(void *data)
{
    if (data == NULL) {
        return HOUND_NULL_VAL;
    }
    s_count = *((__typeof__(s_count) *) data);

    return HOUND_OK;
}

static
hound_err counter_destroy(void)
{
    return HOUND_OK;
}

static
hound_err counter_reset(void *data)
{
    counter_destroy();
    counter_init(data);

    return HOUND_OK;
}

static
hound_err counter_device_name(char *device_name)
{
    strcpy(device_name, s_device_name);

    return HOUND_OK;
}

static
hound_err counter_datadesc(
    struct hound_datadesc **out,
    const char ***schemas,
    hound_data_count *count)
{
    struct hound_datadesc *desc;
    hound_err err;

    XASSERT_NOT_NULL(out);
    XASSERT_NOT_NULL(count);
    XASSERT_NOT_NULL(schemas);

    *count = 1;
    desc = drv_alloc(sizeof(*desc));
    if (desc == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    *schemas = drv_alloc(sizeof(*schemas));
    if (desc == NULL) {
        err = HOUND_OOM;
        goto error_alloc_schemas;
    }
    **schemas = "counter.yaml";

    err = drv_deepcopy_desc(desc, &s_datadesc);
    if (err != HOUND_OK) {
        goto error_deepcopy;
    }

    *out = desc;
    goto out;

error_deepcopy:
    drv_free(schemas);
error_alloc_schemas:
    drv_free(desc);
out:
    return err;
}

static
hound_err counter_setdata(UNUSED const struct hound_data_rq_list *data)
{
    return HOUND_OK;
}

static
hound_err counter_parse(
    uint8_t *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *record_count)
{
    size_t count;
    hound_err err;
    size_t i;
    struct hound_record *record;
    const uint8_t *pos;

    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);
    XASSERT_NOT_NULL(records);

    /* We write full counts, so we should not get partial reads. */
    if (*bytes % sizeof(s_count) != 0) {
        return HOUND_DRIVER_FAIL;
    }

    count = *bytes / sizeof(s_count);
    if (count > HOUND_DRIVER_MAX_RECORDS) {
        count = HOUND_DRIVER_MAX_RECORDS;
    }

    pos = buf;
    for (i = 0; i < count; ++i) {
        record = &records[i];
        record->data = drv_alloc(sizeof(s_count));
        if (record->data == NULL) {
            err = HOUND_OOM;
            goto error_drv_alloc;
        }

        /* We have at least a full record. */
        err = clock_gettime(CLOCK_REALTIME, &record->timestamp);
        XASSERT_EQ(err, 0);
        record->data_id = s_datadesc.data_id;
        record->size = sizeof(s_count);
        memcpy(record->data, pos, sizeof(s_count));
        pos += sizeof(s_count);
    }

    *record_count = count;
    *bytes -= count * sizeof(s_count);

    return HOUND_OK;

error_drv_alloc:
    for (--i; i < count; --i) {
        drv_free(record[i].data);
    }
    return err;
}

static
hound_err counter_start(int *fd)
{
    hound_err err;

    XASSERT_EQ(s_pipe[READ_END], FD_INVALID);
    XASSERT_EQ(s_pipe[WRITE_END], FD_INVALID);

    err = pipe(s_pipe);
    if (err != 0) {
        return err;
    }
    *fd = s_pipe[READ_END];

    return HOUND_OK;
}

static
hound_err counter_stop(void)
{
    hound_err err;

    XASSERT_NEQ(s_pipe[READ_END], FD_INVALID);
    XASSERT_NEQ(s_pipe[WRITE_END], FD_INVALID);

    err = close(s_pipe[READ_END]);
    XASSERT_EQ(err, 0);
    err = close(s_pipe[WRITE_END]);
    XASSERT_EQ(err, 0);

    s_pipe[READ_END] = FD_INVALID;
    s_pipe[WRITE_END] = FD_INVALID;

    return HOUND_OK;
}

static
hound_err counter_next(UNUSED hound_data_id id)
{
    size_t written;

    written = write(s_pipe[WRITE_END], &s_count, sizeof(s_count));
    XASSERT_EQ(written, sizeof(s_count));

    ++s_count;

	return HOUND_OK;
}

void counter_zero(void)
{
    s_count = 0;
}

static struct driver_ops counter_driver = {
    .init = counter_init,
    .destroy = counter_destroy,
    .reset = counter_reset,
    .device_name = counter_device_name,
    .datadesc = counter_datadesc,
    .setdata = counter_setdata,
    .parse = counter_parse,
    .start = counter_start,
    .next = counter_next,
    .stop = counter_stop
};

hound_err register_counter_driver(const char *schema_base, size_t *count)
{
    return driver_register("/dev/counter", &counter_driver, schema_base, count);
}
