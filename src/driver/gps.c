/**
 * @file      gps.c
 * @brief     GPS driver implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <assert.h>
#include <errno.h>
#include <gps.h>
#include <hound/hound.h>
#include <hound/driver/gps.h>
#include <hound-private/driver.h>
#include <hound-private/error.h>
#include <hound-private/log.h>
#include <hound-private/util.h>
#include <stdlib.h>

/* Make sure gps.h looks the way we expect. */
static_assert(
    sizeof(int) == 4,
    "gpsd uses a raw int in its struct gps_fix_t mode field, but int is not "
    "guaranteed to be 4 bytes. Since we want to guarantee an ABI to the user, "
    "we fix it at 4 bytes. Thus if the assumption of a 4-byte int ever "
    "changes, we will need code to handle it.");

struct gps_ctx {
    bool active;
    struct gps_data_t gps;
    char *host;
    char *port;
};

static
hound_err gps_init(
    const char *location,
    UNUSED size_t arg_count,
    UNUSED const struct hound_init_arg *args)
{
    struct gps_ctx *ctx;
    hound_err err;
    struct gps_data_t gps;
    char *host;
    const char *p;
    char *port;
    const char *sep;
    int status;

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
hound_err gps_device_name(char *device_name)
{
    const struct gps_ctx *ctx;

    XASSERT_NOT_NULL(device_name);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    strcpy(device_name, "gps-data");

    return HOUND_OK;
}

static
hound_err gps_datadesc(size_t desc_count, struct drv_datadesc *descs)
{
    struct drv_datadesc *desc;

    XASSERT_EQ(desc_count, 1);
    desc = &descs[0];
    XASSERT_EQ(desc->schema_desc->data_id, HOUND_DATA_GPS);
    desc->enabled = true;
    desc->period_count = 1;
    desc->avail_periods = drv_alloc(sizeof(*desc->avail_periods));
    if (desc->avail_periods == NULL) {
        return HOUND_OOM;
    }
    desc->avail_periods[0] = NSEC_PER_SEC;

    return HOUND_OK;
}

static
hound_err gps_setdata(const struct hound_data_rq *rqs, size_t rqs_len)
{
    XASSERT_NOT_NULL(rqs);
    XASSERT_EQ(rqs_len, 1);
    XASSERT_EQ(rqs[0].id, HOUND_DATA_GPS);

    /* We always yield the same type of data, so there's nothing to do here. */

    return HOUND_OK;
}

#if (GPSD_API_MAJOR_VERSION < 9)
static void
convert_time (double timestamp, struct timespec *ts)
{
  double fraction;

  /* gpsd timestamps as a double value representing UNIX epoch time. */
  ts->tv_sec = timestamp;
  fraction = timestamp - ts->tv_sec;
  ts->tv_nsec = NSEC_PER_SEC * fraction;
}
#else
static void
convert_time (struct timespec ts, struct timespec *out_ts)
{
    /*
     * gpsd versions >= 0 use timespec instead of double, so we don't have to do
     * anything special here.
     */
    *out_ts = ts;
}
#endif

static
void populate_gps_data(struct gps_data *data, struct gps_fix_t *fix)
{
    /*
     * Time uncertainty needs conversion from seconds to nanoseconds, as
     * advertised in the schema.
     */
	data->time_uncertainty = fix->ept * NSEC_PER_SEC;
	data->latitude = fix->latitude;
	data->latitude_uncertainty = fix->epy;
	data->longitude = fix->longitude;
	data->longitude_uncertainty = fix->epx;
	data->altitude = fix->altitude;
	data->altitude_uncertainty = fix->epv;
	data->track = fix->track;
	data->track_uncertainty = fix->epd;
	data->speed = fix->speed;
	data->speed_uncertainty = fix->eps;
	data->climb = fix->climb;
	data->climb_uncertainty = fix->epc;
}

static
hound_err gps_parse(unsigned char *buf, size_t bytes)
{
    struct gps_ctx *ctx;
    struct hound_record record;
    int status;

    XASSERT_NOT_NULL(buf);
    XASSERT_GT(bytes, 0);

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

    record.data = drv_alloc(sizeof(struct gps_data));
    if (record.data == NULL) {
        return HOUND_OOM;
    }
    populate_gps_data((struct gps_data *) record.data, &ctx->gps.fix);
    record.size = sizeof(ctx->gps.fix);

    record.data_id = HOUND_DATA_GPS;
    convert_time(ctx->gps.fix.time, &record.timestamp);

    drv_push_records(&record, 1);

    return HOUND_OK;
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

static struct driver_ops gps_driver = {
    .init = gps_init,
    .destroy = gps_destroy,
    .device_name = gps_device_name,
    .datadesc = gps_datadesc,
    .setdata = gps_setdata,
    .poll = drv_default_push,
    .parse = gps_parse,
    .start = gps_start,
    .next = NULL,
    .stop = gps_stop
};

HOUND_DRIVER_REGISTER_FUNC
static void register_gps_driver(void)
{
    driver_register("gps", &gps_driver);
}
