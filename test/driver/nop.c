/**
 * @file      nop.c
 * @brief     No-op driver implementation. This driver implements all the
 *            required driver functions but does not actually produce data, and
 *            is used for unit-testing the driver core.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <hound-test/id.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#define FD_INVALID (-1)

struct period_desc {
    size_t period_count;
    const hound_data_period *avail_periods;
};

static const hound_data_period s_nop1_period[] = {
    0,
    NSEC_PER_SEC,
    NSEC_PER_SEC/10,
    NSEC_PER_SEC/500,
    NSEC_PER_SEC/1000,
    NSEC_PER_SEC/2000
};
static const hound_data_period s_nop2_period[] = { 0 };
static const struct period_desc s_period_descs[] = {
    {
        .period_count = ARRAYLEN(s_nop1_period),
        .avail_periods = s_nop1_period
    },
    {
        .period_count = ARRAYLEN(s_nop2_period),
        .avail_periods = s_nop2_period
    }
};

struct nop_ctx {
    int fd;
};

hound_err nop_init(
    UNUSED const char *path,
    UNUSED size_t arg_count,
    UNUSED const struct hound_init_arg *args)
{
    struct nop_ctx *ctx;

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return HOUND_OOM;
    }
    ctx->fd = FD_INVALID;

    drv_set_ctx(ctx);

    return HOUND_OK;
}

static
hound_err nop_destroy(void)
{
    free(drv_ctx());

    return HOUND_OK;
}

static
hound_err nop_device_name(char *device_name)
{
    XASSERT_NOT_NULL(device_name);

    strcpy(device_name, "nop");

    return HOUND_OK;
}

static
hound_err nop_datadesc(size_t desc_count, struct drv_datadesc *descs)
{
    struct drv_datadesc *desc;
    const struct period_desc *p_desc;
    size_t i;
    size_t size;

    XASSERT_EQ(desc_count, ARRAYLEN(s_period_descs));

    for (i = 0; i < ARRAYLEN(s_period_descs); ++i) {
        desc = &descs[i];
        p_desc = &s_period_descs[i];
        desc->enabled = true;
        desc->period_count = p_desc->period_count;
        size = p_desc->period_count * sizeof(*p_desc->avail_periods);
        desc->avail_periods = drv_alloc(size);
        if (desc->avail_periods == NULL) {
            for (--i; i < ARRAYLEN(s_period_descs); --i) {
                drv_free(descs[i].avail_periods);
                return HOUND_OOM;
            }
        }
        memcpy(desc->avail_periods, p_desc->avail_periods, size);
    }

    return HOUND_OK;
}

static
hound_err nop_setdata(
    UNUSED const struct hound_data_rq *rqs,
    UNUSED size_t rqs_len)
{
    return HOUND_OK;
}

static
hound_err nop_parse(unsigned char *buf, size_t bytes)
{
    XASSERT_NOT_NULL(buf);
    XASSERT_EQ(bytes, 0);

    return HOUND_OK;
}

static
hound_err nop_start(int *fd)
{
    struct nop_ctx *ctx;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_EQ(ctx->fd, FD_INVALID);
    ctx->fd = open("/dev/null", 0);
    XASSERT_NEQ(ctx->fd, -1);
    *fd = ctx->fd;

    return HOUND_OK;
}

static
hound_err nop_next(UNUSED hound_data_id id)
{
	return HOUND_OK;
}

static
hound_err nop_stop(void)
{
    struct nop_ctx *ctx;
    hound_err err;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_NEQ(ctx->fd, FD_INVALID);
    err = close(ctx->fd);
    XASSERT_NEQ(err, -1);
    ctx->fd = FD_INVALID;

    return HOUND_OK;
}

static struct driver_ops nop_driver = {
    .init = nop_init,
    .destroy = nop_destroy,
    .device_name = nop_device_name,
    .datadesc = nop_datadesc,
    .setdata = nop_setdata,
    .poll = drv_default_push,
    .parse = nop_parse,
    .start = nop_start,
    .next = nop_next,
    .stop = nop_stop
};

HOUND_DRIVER_REGISTER_FUNC
static void register_nop_driver(void)
{
    driver_register("nop", &nop_driver);
}
