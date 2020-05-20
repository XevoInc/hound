/**
 * @file      counter.c
 * @brief     Counter driver implementation. This driver simply increments a
 *            counter every time it is read from in order to do a basic test of
 *            the I/O subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <hound-test/id.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#define FD_INVALID (-1)

#define READ_END (0)
#define WRITE_END (1)

struct counter_ctx {
    int pipe[2];
    uint64_t count;
};

static
hound_err counter_init(
    UNUSED const char *path,
    size_t arg_count,
    const struct hound_init_arg *args)
{
    struct counter_ctx *ctx;

    if (args == NULL) {
        return HOUND_NULL_VAL;
    }
    if (arg_count != 1 || args->type != HOUND_TYPE_UINT64) {
        return HOUND_INVALID_VAL;
    }

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return HOUND_OOM;
    }
    ctx->pipe[READ_END] = FD_INVALID;
    ctx->pipe[WRITE_END] = FD_INVALID;
    ctx->count = args->data.as_uint64;

    drv_set_ctx(ctx);

    return HOUND_OK;
}

static
hound_err counter_destroy(void)
{
    free(drv_ctx());

    return HOUND_OK;
}

static
hound_err counter_device_name(char *device_name)
{
    strcpy(device_name, "counter");

    return HOUND_OK;
}

static
hound_err counter_datadesc(size_t desc_count, struct drv_datadesc *descs)
{
    struct drv_datadesc *desc;

    XASSERT_EQ(desc_count, 1);
    desc = &descs[0];
    desc->enabled = true;
    desc->period_count = 0;
    desc->avail_periods = NULL;

    return HOUND_OK;
}

static
hound_err counter_setdata(UNUSED const struct hound_data_rq_list *rq_list)
{
    return HOUND_OK;
}

static
hound_err counter_parse(
    unsigned char *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *record_count)
{
    size_t count;
    struct counter_ctx *ctx;
    hound_err err;
    size_t i;
    struct hound_record *record;
    const unsigned char *pos;

    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);
    XASSERT_NOT_NULL(records);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    /* We write full counts, so we should not get partial reads. */
    if (*bytes % sizeof(ctx->count) != 0) {
        return HOUND_DRIVER_FAIL;
    }

    count = *bytes / sizeof(ctx->count);
    if (count > HOUND_DRIVER_MAX_RECORDS) {
        count = HOUND_DRIVER_MAX_RECORDS;
    }

    pos = buf;
    for (i = 0; i < count; ++i) {
        record = &records[i];
        record->data = drv_alloc(sizeof(ctx->count));
        if (record->data == NULL) {
            err = HOUND_OOM;
            goto error_drv_alloc;
        }

        /* We have at least a full record. */
        err = clock_gettime(CLOCK_REALTIME, &record->timestamp);
        XASSERT_EQ(err, 0);
        record->data_id = HOUND_DATA_COUNTER;
        record->size = sizeof(ctx->count);
        memcpy(record->data, pos, sizeof(ctx->count));
        pos += sizeof(ctx->count);
    }

    *record_count = count;
    *bytes -= count * sizeof(ctx->count);

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
    struct counter_ctx *ctx;
    hound_err err;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_EQ(ctx->pipe[READ_END], FD_INVALID);
    XASSERT_EQ(ctx->pipe[WRITE_END], FD_INVALID);

    err = pipe(ctx->pipe);
    if (err != 0) {
        return err;
    }
    *fd = ctx->pipe[READ_END];

    return HOUND_OK;
}

#include <sys/ioctl.h>

static
hound_err counter_stop(void)
{
    struct counter_ctx *ctx;
    hound_err err;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_NEQ(ctx->pipe[READ_END], FD_INVALID);
    XASSERT_NEQ(ctx->pipe[WRITE_END], FD_INVALID);

    err = close(ctx->pipe[READ_END]);
    XASSERT_EQ(err, 0);
    err = close(ctx->pipe[WRITE_END]);
    XASSERT_EQ(err, 0);

    ctx->pipe[READ_END] = FD_INVALID;
    ctx->pipe[WRITE_END] = FD_INVALID;

    return HOUND_OK;
}

#include <errno.h>

static
hound_err counter_next(hound_data_id id)
{
    struct counter_ctx *ctx;
    ssize_t written;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_EQ(id, HOUND_DATA_COUNTER);

    errno = 0;
    written = write(ctx->pipe[WRITE_END], &ctx->count, sizeof(ctx->count));
    XASSERT_EQ(written, sizeof(ctx->count));

    ++ctx->count;

	return HOUND_OK;
}

static struct driver_ops counter_driver = {
    .init = counter_init,
    .destroy = counter_destroy,
    .device_name = counter_device_name,
    .datadesc = counter_datadesc,
    .setdata = counter_setdata,
    .poll = drv_default_pull,
    .parse = counter_parse,
    .start = counter_start,
    .next = counter_next,
    .stop = counter_stop
};

HOUND_DRIVER_REGISTER_FUNC
static void register_counter_driver(void)
{
    driver_register("counter", &counter_driver);
}
