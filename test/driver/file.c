/**
 * @file      file.c
 * @brief     Test file driver implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <hound-test/id.h>
#include <linux/limits.h>
#include <unistd.h>

#define FD_INVALID (-1)

#define READ_END (0)
#define WRITE_END (1)

static hound_data_period s_period_ns = 0;
static struct hound_datadesc s_datadesc = {
    .data_id = HOUND_DATA_FILE,
    .period_count = 1,
    .avail_periods = &s_period_ns
};


struct file_ctx {
    char filepath[PATH_MAX];
    int fd;
    int pipe[2];
    char buf[4096];
};

static
hound_err file_init(
    const char *filepath,
    UNUSED size_t arg_count,
    UNUSED const struct hound_init_arg *args)
{
    struct file_ctx *ctx;

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return HOUND_OOM;
    }

    /*
     * PATH_MAX includes '\0', so if the len is PATH_MAX, then no '\0' character
     * was found.
     */
    if (strnlen(filepath, PATH_MAX) == PATH_MAX) {
        return HOUND_INVALID_STRING;
    }
    strcpy(ctx->filepath, filepath);
    ctx->fd = FD_INVALID;
    ctx->pipe[READ_END] = FD_INVALID;
    ctx->pipe[WRITE_END] = FD_INVALID;

    drv_set_ctx(ctx);

    return HOUND_OK;
}

static
hound_err file_destroy(void)
{
    free(drv_ctx());

    return HOUND_OK;
}

static
hound_err file_device_name(char *device_name)
{
    XASSERT_NOT_NULL(device_name);

    strcpy(device_name, "file");

    return HOUND_OK;
}

static
hound_err file_datadesc(
    size_t desc_count,
    struct drv_datadesc *descs,
    drv_sched_mode *mode)
{
    struct drv_datadesc *desc;

    XASSERT_EQ(desc_count, 1);
    desc = &descs[0];
    desc->enabled = true;
    desc->period_count = 1;
    desc->avail_periods = malloc(sizeof(*desc));
    if (desc->avail_periods == NULL) {
        return HOUND_OOM;
    }
    desc->avail_periods[0] = 0;

    *mode = DRV_SCHED_PUSH;

    return HOUND_OK;
}

static
hound_err file_setdata(const struct hound_data_rq_list *data)
{
    const struct hound_data_rq *rq;

    XASSERT_NOT_NULL(data);
    XASSERT_EQ(data->len, 1);
    XASSERT_NOT_NULL(data->data);

    rq = data->data;
    XASSERT_EQ(rq->id, s_datadesc.data_id);
    XASSERT_EQ(rq->period_ns, s_datadesc.avail_periods[0]);

    return HOUND_OK;
}

static
hound_err file_parse(
    unsigned char *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *record_count)
{
    hound_err err;
    struct timespec timestamp;
    struct hound_record *record;

    err = clock_gettime(CLOCK_REALTIME, &timestamp);
    XASSERT_EQ(err, 0);

    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);

    record = records;
    record->data = drv_alloc(*bytes * sizeof(*buf));
    if (record->data == NULL) {
        return HOUND_OOM;
    }

    memcpy(record->data, buf, *bytes);
    record->data_id = s_datadesc.data_id;
    record->timestamp = timestamp;
    record->size = *bytes;

    *record_count = 1;
    *bytes = 0;

    return HOUND_OK;
}

static
hound_err file_next(hound_data_id id)
{
    ssize_t bytes;
    struct file_ctx *ctx;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_EQ(id, HOUND_DATA_FILE);

    bytes = read(ctx->fd, ctx->buf, ARRAYLEN(ctx->buf));
    XASSERT_NEQ(bytes, -1);
    if (bytes == 0) {
        /* End of file. */
        return HOUND_OK;
    }

    bytes = write(ctx->pipe[WRITE_END], ctx->buf, bytes);
    XASSERT_NEQ(bytes, -1);

    return HOUND_OK;
}

static
hound_err file_start(int *out_fd)
{
    struct file_ctx *ctx;
    hound_err err;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_NOT_NULL(out_fd);
    XASSERT_EQ(ctx->pipe[READ_END], FD_INVALID);
    XASSERT_EQ(ctx->pipe[WRITE_END], FD_INVALID);

    err = open(ctx->filepath, 0, O_RDONLY);
    if (err == -1) {
        err = errno;
        goto out;
    }
    ctx->fd = err;

    err = pipe(ctx->pipe);
    if (err == -1) {
        goto error_pipe_fail;
    }

    *out_fd = ctx->pipe[READ_END];
    err = HOUND_OK;

    goto out;

error_pipe_fail:
    close(ctx->fd);
    ctx->fd = FD_INVALID;
out:
    return err;
}

static
hound_err file_stop(void)
{
    struct file_ctx *ctx;
    hound_err err;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_NEQ(ctx->fd, FD_INVALID);
    XASSERT_NEQ(ctx->pipe[READ_END], FD_INVALID);
    XASSERT_NEQ(ctx->pipe[WRITE_END], FD_INVALID);

    err = close(ctx->fd);
    if (err != -1) {
        return err;
    }

    return HOUND_OK;
}

static struct driver_ops file_driver = {
    .init = file_init,
    .destroy = file_destroy,
    .device_name = file_device_name,
    .datadesc = file_datadesc,
    .setdata = file_setdata,
    .parse = file_parse,
    .start = file_start,
    .next = file_next,
    .stop = file_stop
};

HOUND_DRIVER_REGISTER_FUNC
static void register_file_driver(void)
{
    driver_register("file", &file_driver);
}
