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
#include <hound-private/api.h>
#include <hound-private/driver.h>
#include <hound-private/driver/util.h>
#include <hound-test/assert.h>
#include <hound-test/id.h>
#include <linux/limits.h>
#include <string.h>
#include <unistd.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define NSEC_PER_SEC ((hound_data_period) 1e9)
#define FD_INVALID (-1)
#define UNUSED __attribute__((unused))

#define READ_END (0)
#define WRITE_END (1)

struct driver_init {
    const char *filepath;
    hound_data_id data_id;
};

static const char *s_device_name = "file";
static hound_data_period s_period_ns = 0;
static const char *s_filepath = NULL;
static struct hound_datadesc s_datadesc = {
    .data_id = HOUND_DATA_FILE,
    .period_count = 1,
    .avail_periods = &s_period_ns
};


static int s_fd;
static int s_pipe[2];
static char s_file_buf[4096];

static
hound_err file_init(void *data)
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
    s_datadesc.data_id = init->data_id;
    s_fd = FD_INVALID;
    s_pipe[READ_END] = FD_INVALID;
    s_pipe[WRITE_END] = FD_INVALID;

    return HOUND_OK;
}

static
hound_err file_destroy(void)
{
    s_filepath = NULL;

    return HOUND_OK;
}

static
hound_err file_reset(void *data)
{
    file_destroy();
    file_init(data);

    return HOUND_OK;
}

static
hound_err file_device_name(char *device_name)
{
    XASSERT_NOT_NULL(device_name);

    strcpy(device_name, s_device_name);

    return HOUND_OK;
}

static
hound_err file_datadesc(
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
    **schemas = "file.yaml";

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
    uint8_t *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *record_count)
{
    hound_err err;
    struct timespec timestamp;
    struct hound_record *record;

    err = clock_gettime(CLOCK_MONOTONIC, &timestamp);
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

static
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

static
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
    .device_name = file_device_name,
    .datadesc = file_datadesc,
    .setdata = file_setdata,
    .parse = file_parse,
    .start = file_start,
	.next = file_next,
    .stop = file_stop
};

PUBLIC_API
hound_err hound_register_file_driver(
    const char *filepath,
    const char *schema_base,
    hound_data_id id)
{
	struct driver_init init;

	init.filepath = filepath;
	init.data_id = id;
	return driver_register(filepath, &file_driver, schema_base, &init);
}
