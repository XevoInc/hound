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

#define CTX_MAGIC ((void *) 0x1ceb00da)
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
static int s_fd = FD_INVALID;

hound_err nop_init(UNUSED void *data)
{
    void *ctx;

    ctx = drv_ctx();
    XASSERT_NULL(ctx);
    drv_set_ctx(CTX_MAGIC);

    return HOUND_OK;
}

static
hound_err nop_destroy(void)
{
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
    struct hound_datadesc **out,
    const char ***schemas,
    hound_data_count *count,
    drv_sched_mode *mode)
{
    struct hound_datadesc *desc;
    hound_err err;
    size_t i;

    XASSERT_NOT_NULL(out);
    XASSERT_NOT_NULL(schemas);
    XASSERT_NOT_NULL(count);

    *count = ARRAYLEN(s_datadesc);
    desc = drv_alloc(*count*sizeof(*desc));
    if (desc == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    *schemas = drv_alloc(sizeof(**schemas));
    if (*schemas == NULL) {
        err = HOUND_OOM;
        goto error_schema_alloc;
    }

    for (i = 0; i < ARRAYLEN(s_datadesc); ++i) {
        err = drv_deepcopy_desc(&desc[i], &s_datadesc[i]);
        if (err != HOUND_OK) {
            goto error_deepcopy;
        }
        (*schemas)[i] = "nop.yaml";
    }

    *mode = DRV_SCHED_PUSH;
    *out = desc;
    return HOUND_OK;

error_deepcopy:
    for (; i < *count; --i) {
        drv_destroy_desc(&desc[i]);
    }
    drv_free(desc);
error_schema_alloc:
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
    void *ctx;

    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_EQ(*bytes, 0);
    XASSERT_NOT_NULL(records);
    XASSERT_NOT_NULL(record_count);

    ctx = drv_ctx();
    XASSERT_EQ(ctx, CTX_MAGIC);

    *record_count = 0;

    return HOUND_OK;
}

static
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

static
hound_err nop_next(UNUSED hound_data_id id)
{
	return HOUND_OK;
}

static
hound_err nop_stop(void)
{
    hound_err err;

    XASSERT_NEQ(s_fd, FD_INVALID);
    err = close(s_fd);
    XASSERT_NEQ(err, -1);

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
