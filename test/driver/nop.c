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
#include <hound-private/driver/util.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <hound-test/id.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#define FD_INVALID (-1)

static const char *s_device_name = "nop";
static const hound_data_period s_nop1_period[] = {
    0,
    NSEC_PER_SEC,
    NSEC_PER_SEC/10,
    NSEC_PER_SEC/500,
    NSEC_PER_SEC/1000,
    NSEC_PER_SEC/2000
};
static const hound_data_period s_nop2_period[] = { 0 };
static const struct hound_datadesc s_datadesc[] = {
    {
        .data_id = HOUND_DATA_NOP1,
        .period_count = ARRAYLEN(s_nop1_period),
        .avail_periods = s_nop1_period
    },
    {
        .data_id = HOUND_DATA_NOP2,
        .period_count = ARRAYLEN(s_nop2_period),
        .avail_periods = s_nop2_period
    }
};

struct nop_ctx {
    int fd;
};

hound_err nop_init(UNUSED void *data)
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

    strcpy(device_name, s_device_name);

    return HOUND_OK;
}

static
hound_err nop_datadesc(
    hound_data_count *desc_count,
    struct hound_datadesc **out_descs,
    char *schema,
    drv_sched_mode *mode)
{
    struct hound_datadesc *desc;
    hound_err err;
    size_t i;

    XASSERT_NOT_NULL(desc_count);
    XASSERT_NOT_NULL(out_descs);
    XASSERT_NOT_NULL(schema);

    *desc_count = ARRAYLEN(s_datadesc);
    desc = drv_alloc(*desc_count*sizeof(*desc));
    if (desc == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    strcpy(schema, "nop.yaml");

    for (i = 0; i < ARRAYLEN(s_datadesc); ++i) {
        err = drv_deepcopy_desc(&desc[i], &s_datadesc[i]);
        if (err != HOUND_OK) {
            for (--i; i < ARRAYLEN(s_datadesc); --i) {
                drv_destroy_desc(&desc[i]);
            }
            goto error_deepcopy;
        }
    }

    *mode = DRV_SCHED_PUSH;
    *out_descs = desc;
    return HOUND_OK;

error_deepcopy:
    drv_free(desc);
out:
    return err;
}

static
hound_err nop_setdata(UNUSED const struct hound_data_rq_list *data)
{
    return HOUND_OK;
}

static
hound_err nop_parse(
    uint8_t *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *record_count)
{
    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_EQ(*bytes, 0);
    XASSERT_NOT_NULL(records);
    XASSERT_NOT_NULL(record_count);

    *record_count = 0;

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
    .parse = nop_parse,
    .start = nop_start,
    .next = nop_next,
    .stop = nop_stop
};

hound_err register_nop_driver(const char *schema_base)
{
    return driver_register("/dev/nop", &nop_driver, schema_base, NULL);
}
