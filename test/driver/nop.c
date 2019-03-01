/**
 * @file      nop.c
 * @brief     No-op driver implementation. This driver implements all the
 *            required driver functions but does not actually produce data, and
 *            is used for unit-testing the driver core.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/assert.h>
#include <hound/hound.h>
#include <hound/driver.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define FD_INVALID (-1)
#define UNUSED __attribute__((unused))

static const char *s_device_ids[] = {"dummy", "fake"};
static const hound_data_freq s_accel_freq[] = { 0, 1, 10, 500, 1000, 2000 };
static const hound_data_freq s_gyro_freq[] = { 0 };
static struct hound_drv_datadesc s_datadesc[] = {
    {
        .id = HOUND_DEVICE_ACCELEROMETER,
        .name = "super-extra-accelerometer",
        .freq_count = ARRAYLEN(s_accel_freq),
        .avail_freq = s_accel_freq
    },

    {
        .id = HOUND_DEVICE_GYROSCOPE,
        .name = "oneshot-gyroscope",
        .freq_count = ARRAYLEN(s_gyro_freq),
        .avail_freq = s_gyro_freq
    }
};
static int s_fd = FD_INVALID;

hound_err nop_init(UNUSED hound_alloc alloc)
{
    return HOUND_OK;
}

hound_err nop_destroy(void)
{
    return HOUND_OK;
}

hound_err nop_reset(void)
{
    return HOUND_OK;
}

hound_err nop_device_ids(
        const char ***device_ids,
        hound_device_id_count *count)
{
    *count = ARRAYLEN(s_device_ids);
    *device_ids = s_device_ids;

    return HOUND_OK;
}

hound_err nop_datadesc(
        const struct hound_drv_datadesc **desc,
        hound_datacount *count)
{
    *count = ARRAYLEN(s_datadesc);
    *desc = s_datadesc;

    return HOUND_OK;
}

hound_err nop_setdata(UNUSED const struct hound_drv_data_list *data)
{
    return HOUND_OK;
}

hound_err nop_parse(
    const uint8_t *buf,
    size_t *bytes,
    struct hound_record *record)
{
    HOUND_ASSERT_NOT_NULL(buf);
    HOUND_ASSERT_NOT_NULL(bytes);
    HOUND_ASSERT_GT(*bytes, 0);
    HOUND_ASSERT_NOT_NULL(record);

    return HOUND_OK;
}

hound_err nop_start(int *fd)
{
    HOUND_ASSERT_EQ(s_fd, FD_INVALID);
    s_fd = open("/dev/null", 0);
    HOUND_ASSERT_NEQ(s_fd, -1);
    *fd = s_fd;

    return HOUND_OK;
}

hound_err nop_stop(void)
{
    hound_err err;

    HOUND_ASSERT_NEQ(s_fd, FD_INVALID);
    err = close(s_fd);
    HOUND_ASSERT_NEQ(err, -1);

    return HOUND_OK;
}

struct hound_io_driver nop_driver = {
    .init = nop_init,
    .destroy = nop_destroy,
    .reset = nop_reset,
    .device_ids = nop_device_ids,
    .datadesc = nop_datadesc,
    .setdata = nop_setdata,
    .parse = nop_parse,
    .start = nop_start,
    .stop = nop_stop
};
