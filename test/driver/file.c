/**
 * @file      file.c
 * @brief     Test file driver implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <hound/hound.h>
#include <hound_private/api.h>
#include <hound_private/driver.h>
#include <hound_test/assert.h>
#include <linux/limits.h>
#include <string.h>
#include <unistd.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define NS_PER_SEC ((hound_data_period) 1e9)
#define FD_INVALID (-1)
#define UNUSED __attribute__((unused))

#define READ_END (0)
#define WRITE_END (1)

struct driver_init {
    const char *filepath;
    hound_data_id data_id;
};

static const char *s_device_ids[] = {"file"};
static hound_data_period s_period_ns = 0;
static const char *s_filepath = NULL;
static struct hound_drv_datadesc s_datadesc = {
    .name = "file-data",
    .period_count = 1,
    .avail_periods = &s_period_ns
};

static hound_alloc *s_alloc;

static int s_fd;
static int s_pipe[2];
static char s_file_buf[4096];

hound_err file_init(hound_alloc alloc, void *data)
{
    struct driver_init *init;

    if (data == NULL) {
        return HOUND_NULL_VAL;
    }
    init = data;

    /*
     * PATH_MAX includes '\0', so if the len is PATH_MAX, then no '\0' character
     * was found.
     */
    if (strnlen(init->filepath, PATH_MAX) == PATH_MAX) {
        return HOUND_INVALID_STRING;
    }
    s_filepath = init->filepath;
    s_datadesc.id = init->data_id;
    s_fd = FD_INVALID;
    s_pipe[READ_END] = FD_INVALID;
    s_pipe[WRITE_END] = FD_INVALID;
    s_alloc = alloc;

    return HOUND_OK;
}

hound_err file_destroy(void)
{
    s_alloc = NULL;
    s_filepath = NULL;

    return HOUND_OK;
}

hound_err file_reset(hound_alloc alloc, void *data)
{
    file_destroy();
    file_init(alloc, data);

    return HOUND_OK;
}

hound_err file_device_ids(
    const char ***device_ids,
    hound_device_id_count *count)
{
    XASSERT_NOT_NULL(device_ids);
    XASSERT_NOT_NULL(count);

    *device_ids = s_device_ids;
    *count = ARRAYLEN(s_device_ids);

    return HOUND_OK;
}

hound_err file_datadesc(
    const struct hound_drv_datadesc **desc,
    hound_data_count *count)
{
    XASSERT_NOT_NULL(desc);
    XASSERT_NOT_NULL(count);

    *desc = &s_datadesc;
    *count = 1;

    return HOUND_OK;
}

hound_err file_setdata(const struct hound_drv_data_list *data)
{
    const struct hound_drv_data *drv_data;

    XASSERT_NOT_NULL(data);
    XASSERT_EQ(data->len, 1);
    XASSERT_NOT_NULL(data->data);

    drv_data = data->data;
    XASSERT_EQ(drv_data->id, s_datadesc.id);
    XASSERT_EQ(drv_data->period_ns, s_datadesc.avail_periods[0]);

    return HOUND_OK;
}

hound_err file_parse(
    const uint8_t *buf,
    size_t *bytes,
    struct hound_record *record)
{
    hound_err err;
    struct timespec timestamp;

    err = clock_gettime(CLOCK_MONOTONIC, &timestamp);
    XASSERT_EQ(err, 0);

    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);

    record->data = malloc(*bytes * sizeof(*buf));
    if (record->data == NULL) {
        return HOUND_OOM;
    }
    memcpy(record->data, buf, *bytes);
    record->id = s_datadesc.id;
    record->timestamp = timestamp;
    record->size = *bytes;

    *bytes = 0;

    return HOUND_OK;
}

hound_err file_next(UNUSED hound_data_id id)
{
    ssize_t bytes;

	bytes = read(s_fd, s_file_buf, ARRAYLEN(s_file_buf));
	XASSERT_NEQ(bytes, -1);
	if (bytes == 0) {
		/* End of file. */
		return HOUND_OK;
	}

	bytes = write(s_pipe[WRITE_END], s_file_buf, bytes);
	XASSERT_NEQ(bytes, -1);

	return HOUND_OK;
}

hound_err file_start(int *out_fd)
{
    hound_err err;

    XASSERT_NOT_NULL(out_fd);
    XASSERT_EQ(s_pipe[READ_END], FD_INVALID);
    XASSERT_EQ(s_pipe[WRITE_END], FD_INVALID);

    err = open(s_filepath, 0, O_RDONLY);
    if (err == -1) {
        err = errno;
        goto out;
    }
    s_fd = err;

    err = pipe(s_pipe);
    if (err == -1) {
        goto error_pipe_fail;
    }

    *out_fd = s_pipe[READ_END];
    err = HOUND_OK;

    goto out;

error_pipe_fail:
    close(s_fd);
    s_fd = FD_INVALID;
out:
    return err;
}

hound_err file_stop(void)
{
    hound_err err;

    XASSERT_NEQ(s_fd, FD_INVALID);
    XASSERT_NEQ(s_pipe[READ_END], FD_INVALID);
    XASSERT_NEQ(s_pipe[WRITE_END], FD_INVALID);

    err = close(s_fd);
    if (err != -1) {
        return err;
    }

    return HOUND_OK;
}

static struct driver_ops file_driver = {
    .init = file_init,
    .destroy = file_destroy,
    .reset = file_reset,
    .device_ids = file_device_ids,
    .datadesc = file_datadesc,
    .setdata = file_setdata,
    .parse = file_parse,
    .start = file_start,
	.next = file_next,
    .stop = file_stop
};

PUBLIC_API
hound_err hound_register_file_driver(const char *filepath, hound_data_id id)
{
	struct driver_init init;

	init.filepath = filepath;
	init.data_id = id;
	return driver_register(filepath, &file_driver, &init);
}
