/**
 * @file      gps.c
 * @brief     GPS driver implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2018 Xevo Inc. All Rights Reserved.
 */

#include <assert.h>
#include <errno.h>
#include <gps.h>
#include <hound/hound.h>
#include <hound_private/api.h>
#include <hound_private/driver.h>
#include <hound_private/driver/util.h>
#include <hound_private/error.h>
#include <hound_private/log.h>
#include <stdlib.h>
#include <string.h>

/* Make sure gps.h looks the way we expect. */
static_assert(
    sizeof(int) == 4,
    "gpsd uses a raw int in its struct gps_fix_t mode field, but int is not "
    "guaranteed to be 4 bytes. Since we want to guarantee an ABI to the user, "
    "we fix it at 4 bytes. Thus if the assumption of a 4-byte int ever "
    "changes, we will need code to handle it.");

#define NSEC_PER_SEC ((uint64_t) 1e9)

/* GPS always yields data once per second. */
static hound_data_period s_avail_periods = NSEC_PER_SEC;
static struct hound_datadesc s_datadesc = {
    .name = "gps-data",
    .id = HOUND_DEVICE_GPS,
    .period_count = 1,
    .avail_periods = &s_avail_periods
};

struct gps_ctx {
    bool active;
    struct gps_data_t gps;
    char *host;
    char *port;
};

static
hound_err gps_init(void *data)
{
    struct gps_ctx *ctx;
    hound_err err;
    struct gps_data_t gps;
    char *host;
    const char *location;
    const char *p;
    char *port;
    const char *sep;
    int status;

    if (data == NULL) {
        return HOUND_NULL_VAL;
    }
    location = data;

    for (p = location; *p != '\0' && *p != ':'; ++p);
    if (*p == '\0') {
        /* No ':' separator. */
        return HOUND_INVALID_STRING;
    }
    sep = p;
    for (++p; *p != '\0'; ++p);

    host = malloc(sep - location + 1);
    if (host == NULL) {
        err = HOUND_OOM;
        goto out;
    }
    memcpy(host, location, sep - location);
    host[sep - location] = '\0';

    port = malloc(p - sep);
    if (host == NULL) {
        err = HOUND_OOM;
        goto error_alloc_port;
    }
    memcpy(port, sep+1, p-(sep+1));
    port[p - (sep+1)] = '\0';

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        err = HOUND_OOM;
        goto error_alloc_ctx;
    }

    /* Make sure this is a valid host-port combination. */
    status = gps_open(host, port, &gps);
    if (status != 0) {
        err = errno;
        goto error_gps_open;
    }

    status = gps_close(&gps);
    if (status != 0) {
        err = errno;
        goto error_gps_close;
    }

    ctx->host = host;
    ctx->port = port;
    ctx->active = false;

    drv_set_ctx(ctx);

    err = HOUND_OK;
    goto out;

error_gps_close:
error_gps_open:
error_alloc_ctx:
    free(port);
error_alloc_port:
    free(host);
out:
    return err;
}

static
hound_err gps_destroy(void)
{
    struct gps_ctx *ctx;

    ctx = drv_ctx();
    if (ctx != NULL) {
        free(ctx->host);
        free(ctx->port);
        free(ctx);
    }

    return HOUND_OK;
}

static
hound_err gps_device_id(char *device_id)
{
    const struct gps_ctx *ctx;

    XASSERT_NOT_NULL(device_id);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    strcpy(device_id, "gps-data");

    return HOUND_OK;
}

static
hound_err gps_datadesc(struct hound_datadesc **out, hound_data_count *count)
{
    struct hound_datadesc *desc;
    hound_err err;

    XASSERT_NOT_NULL(out);
    XASSERT_NOT_NULL(count);

    *count = 1;
    desc = drv_alloc(sizeof(*desc));
    if (desc == NULL) {
        return HOUND_OOM;
    }
    err = drv_deepcopy_desc(desc, &s_datadesc);
    if (err != HOUND_OK) {
        drv_free(desc);
    }

    *out = desc;
    return err;
}

static
hound_err gps_setdata(const struct hound_data_rq_list *data_list)
{
    XASSERT_NOT_NULL(data_list);
    XASSERT_EQ(data_list->len, 1);
    XASSERT_NOT_NULL(data_list->data);

    /* We always yield the same type of data, so there's nothing to do here. */

    return HOUND_OK;
}

static void
unix_to_timespec (double timestamp, struct timespec *ts)
{
  double fraction;

  /* gpsd timestamps as a double value representing UNIX epoch time. */
  ts->tv_sec = timestamp;
  fraction = timestamp - ts->tv_sec;
  ts->tv_nsec = NSEC_PER_SEC * fraction;
}

static
hound_err gps_parse(
    uint8_t *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *record_count)
{
    struct gps_ctx *ctx;
    struct hound_record *record;
    int status;

    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);
    XASSERT_NOT_NULL(records);
    XASSERT_NOT_NULL(record_count);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    status = gps_unpack((char *) buf, &ctx->gps);
    if (status != 0) {
        return errno;
    }

    if (ctx->gps.status == STATUS_NO_FIX ||
        ctx->gps.fix.mode == MODE_NO_FIX) {
        return HOUND_OK;
    }

    record = records;
    record->data = drv_alloc(sizeof(ctx->gps.fix));
    if (record->data == NULL) {
        return HOUND_OOM;
    }
    memcpy(record->data, &ctx->gps.fix, sizeof(ctx->gps.fix));
    record->size = sizeof(ctx->gps.fix);

    record->id = HOUND_DEVICE_GPS;
    unix_to_timespec(ctx->gps.fix.time, &record->timestamp);

    *record_count = 1;
    *bytes = 0;

    return HOUND_OK;
}

static
hound_err gps_next(hound_data_id id)
{
    const struct gps_ctx *ctx;

    XASSERT_EQ(id, HOUND_DEVICE_CAN);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    /*
     * We don't support one-shot data, since we can't control when we get GPS
     * samples.
     */
    return HOUND_DRIVER_UNSUPPORTED;
}

static
hound_err gps_start(int *out_fd)
{
    struct gps_ctx *ctx;
    hound_err err;
    int status;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT(!ctx->active);

    status = gps_open(ctx->host, ctx->port, &ctx->gps);
    if (status != 0) {
        err = errno;
        goto out;
    }

    /*
     * We must use JSON because it's the only thing gps_unpack can parse. We
     * must use gps_unpack because we do the poll and read ourselves in the
     * driver core, rather than using the gps_read function.
     */
    status = gps_stream(&ctx->gps, WATCH_ENABLE | WATCH_JSON, NULL);
    if (status != 0) {
        err = errno;
        goto error_gps_stream;
    }

    *out_fd = ctx->gps.gps_fd;
    ctx->active = true;

    err = HOUND_OK;
    goto out;

error_gps_stream:
    status = gps_close(&ctx->gps);
    if (status != 0) {
        hound_log_err(
            err,
            "failed to close GPS device: %d (%s)\n",
            errno,
            gps_errstr(errno));
    }
out:
    return err;
}

static
hound_err gps_stop(void)
{
    struct gps_ctx *ctx;
    int status;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT(ctx->active);

    status = gps_close(&ctx->gps);
    if (status != 0) {
        return errno;
    }

    ctx->active = false;

    return HOUND_OK;
}

static
hound_err gps_reset(void *data)
{
    struct gps_ctx *ctx;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    if (ctx->active) {
        gps_stop();
    }
    gps_destroy();
    gps_init(data);

    return HOUND_OK;
}

static struct driver_ops gps_driver = {
    .init = gps_init,
    .destroy = gps_destroy,
    .reset = gps_reset,
    .device_id = gps_device_id,
    .datadesc = gps_datadesc,
    .setdata = gps_setdata,
    .parse = gps_parse,
    .start = gps_start,
    .next = gps_next,
    .stop = gps_stop
};

PUBLIC_API
hound_err hound_register_gps_driver(const char *location)
{
    if (location == NULL) {
        return HOUND_NULL_VAL;
    }

    return driver_register(location, &gps_driver, (void *) location);
}
