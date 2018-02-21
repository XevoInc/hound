/**
 * @file      nop.c
 * @brief     No-op driver implementation. This driver implements all the
 *            required driver functions but does not actually produce data, and
 *            is used for unit-testing the driver core.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound_private/driver.h>
#include <hound_private/driver/util.h>
#include <hound_test/assert.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define CTX_MAGIC ((void *) 0x1ceb00da)
#define NS_PER_SEC (1e9)
#define FD_INVALID (-1)
#define UNUSED __attribute__((unused))

static const char *s_device_id = "dummy";
static const hound_data_period s_accel_period[] = {
    0,
    NS_PER_SEC,
    NS_PER_SEC/10,
    NS_PER_SEC/500,
    NS_PER_SEC/1000,
    NS_PER_SEC/2000
};
static const hound_data_period s_gyro_period[] = { 0 };
static const struct hound_datadesc s_datadesc[] = {
    {
        .id = HOUND_DEVICE_ACCELEROMETER,
        .name = "super-extra-accelerometer",
        .period_count = ARRAYLEN(s_accel_period),
        .avail_periods = s_accel_period
    },
    {
        .id = HOUND_DEVICE_GYROSCOPE,
        .name = "oneshot-gyroscope",
        .period_count = ARRAYLEN(s_gyro_period),
        .avail_periods = s_gyro_period
    }
};
static int s_fd = FD_INVALID;

hound_err nop_init(UNUSED void *data)
{
    void *ctx;

    ctx = drv_ctx();
    XASSERT_NULL(ctx);
    drv_set_ctx(CTX_MAGIC);

    return HOUND_OK;
}

hound_err nop_destroy(void)
{
    return HOUND_OK;
}

hound_err nop_reset(UNUSED void *data)
{
    return HOUND_OK;
}

hound_err nop_device_id(char *device_id)
{
    XASSERT_NOT_NULL(device_id);

    strcpy(device_id, s_device_id);

    return HOUND_OK;
}

hound_err nop_datadesc(struct hound_datadesc **out, hound_data_count *count)
{
    struct hound_datadesc *desc;
    hound_err err;
    size_t i;

    XASSERT_NOT_NULL(out);
    XASSERT_NOT_NULL(count);

    *count = ARRAYLEN(s_datadesc);
    desc = drv_alloc(*count*sizeof(*desc));
    if (desc == NULL) {
        err = HOUND_OOM;
        goto error_desc;
    }

    for (i = 0; i < ARRAYLEN(s_datadesc); ++i) {
        err = drv_deepcopy_desc(&desc[i], &s_datadesc[i]);
        if (err != HOUND_OK) {
            goto error_deepcopy;
        }
    }

    *out = desc;
    return HOUND_OK;

error_deepcopy:
    for (; i < *count; --i) {
        drv_destroy_desc(&desc[i]);
    }
    drv_free(desc);
error_desc:
    return err;
}

hound_err nop_setdata(UNUSED const struct hound_data_rq_list *data)
{
    return HOUND_OK;
}

hound_err nop_parse(
    const uint8_t *buf,
    size_t *bytes,
    struct hound_record *record)
{
    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);
    XASSERT_NOT_NULL(record);

    return HOUND_OK;
}

hound_err nop_start(int *fd)
{
    void *ctx;

    XASSERT_EQ(s_fd, FD_INVALID);
    s_fd = open("/dev/null", 0);
    XASSERT_NEQ(s_fd, -1);
    *fd = s_fd;

    /* Make sure the context we set in init is still set. */
    ctx = drv_ctx();
    XASSERT_EQ(ctx, CTX_MAGIC);

    return HOUND_OK;
}

hound_err nop_next(UNUSED hound_data_id id)
{
	return HOUND_OK;
}

hound_err nop_stop(void)
{
    hound_err err;

    XASSERT_NEQ(s_fd, FD_INVALID);
    err = close(s_fd);
    XASSERT_NEQ(err, -1);

    return HOUND_OK;
}

struct driver_ops nop_driver = {
    .init = nop_init,
    .destroy = nop_destroy,
    .reset = nop_reset,
    .device_id = nop_device_id,
    .datadesc = nop_datadesc,
    .setdata = nop_setdata,
    .parse = nop_parse,
    .start = nop_start,
    .next = nop_next,
    .stop = nop_stop
};

hound_err register_nop_driver(void)
{
    return driver_register("/dev/nop", &nop_driver, NULL);
}
