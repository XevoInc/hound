/**
 * @file      gps.c
 * @brief     Test for the GPS driver, intended to be run manually by humans.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <gps.h>
#include <hound/driver/gps.h>
#include <hound/hound.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <signal.h>
#include <string.h>

#define DUMP(fix, field, desc) \
    printf("%s: %g\n", desc, fix->field)

static volatile sig_atomic_t s_sig_pending = 0;
void sig_handler(int sig)
{
    s_sig_pending = sig;
}

static
void dump_timestamp(timestamp_t ts)
{
    time_t time;

    time = (time_t) ts;
    printf("time: %s", ctime(&time));
}

static
void dump_fix(struct gps_fix_t *fix)
{
    if (fix->mode == MODE_NOT_SEEN) {
        return;
    }

    /* See gps.h (shipped with gpsd) for details. */
    dump_timestamp(fix->time);
    DUMP(fix, ept, "expected time uncertainty");

    if (fix->mode >= MODE_2D) {
        DUMP(fix, latitude, "latitude (degrees)");
        DUMP(fix, epy, "latitude position uncertainty (meters)");
        DUMP(fix, longitude, "longitude (degrees)");
        DUMP(fix, epx, "longitude position uncertainty (meters)");
        DUMP(fix, track, "course made good (relative to true north)");
        DUMP(fix, epd, "track uncertainty (degrees)");
        DUMP(fix, speed, "speed over ground, (m/s)");
        DUMP(fix, eps, "speed uncertainty, (m/s)");
    }

    if (fix->mode >= MODE_3D) {
        DUMP(fix, altitude, "altitude (meters)");
        DUMP(fix, epv, "vertical position uncertainty (meters)");
        DUMP(fix, climb, "vertical speed, (m/s)");
        DUMP(fix, epc, "vertical speed uncertainty");
    }
}

static
void usage(const char **argv)
{
    fprintf(stderr, "Usage: %s HOST:PORT\n", argv[0]);
}

static
void data_cb(const struct hound_record *record, UNUSED void *ctx)
{
    struct gps_fix_t *fix;

    XASSERT_EQ(record->data_id, HOUND_DATA_GPS);
    XASSERT_EQ(record->size, sizeof(*fix));
    fix = (__typeof(fix)) record->data;

    dump_fix(fix);
}

int main(int argc, const char **argv)
{
    struct sigaction act;
    struct hound_ctx *ctx;
    struct hound_datadesc *desc;
    hound_err err;
    size_t len;
    const char *location;
    struct hound_data_rq data_rq = {
        .id = HOUND_DATA_GPS,
        .period_ns = NSEC_PER_SEC
    };
    struct hound_rq rq = {
        .queue_len = 1000,
        .cb = data_cb,
        .cb_ctx = NULL,
        .rq_list = {
            .len = 1,
            .data = &data_rq
        }
    };

    if (argc != 2) {
        usage(argv);
        exit(EXIT_FAILURE);
    }
    location = argv[1];

    err = hound_init_driver("gps", location, NULL, 0, NULL);
    XASSERT_OK(err);

    err = hound_get_datadesc(&desc, &len);
    XASSERT_OK(err);
    XASSERT_NOT_NULL(desc);
    XASSERT_EQ(len, 1);
    XASSERT_EQ(desc->data_id, HOUND_DATA_GPS);
    XASSERT_STREQ(desc->name, "gps-data");
    XASSERT_EQ(desc->period_count, 1);
    XASSERT_EQ(*desc->avail_periods, NSEC_PER_SEC);
    hound_free_datadesc(desc);

    err = hound_alloc_ctx(&ctx, &rq);
    XASSERT_OK(err);
    XASSERT_NOT_NULL(ctx);

    err = hound_start(ctx);
    XASSERT_OK(err);

    /* Keep reading until we get a signal to exit. */
    act.sa_handler = sig_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    err = sigaction(SIGINT, &act, NULL);
    XASSERT_EQ(err, 0);
    err = sigaction(SIGQUIT, &act, NULL);
    XASSERT_EQ(err, 0);
    err = sigaction(SIGTERM, &act, NULL);
    XASSERT_EQ(err, 0);
    err = sigaction(SIGTSTP, &act, NULL);
    XASSERT_EQ(err, 0);
    while (s_sig_pending == 0) {
        err = hound_read(ctx, 1);
        XASSERT_OK(err);
    }

    err = hound_stop(ctx);
    XASSERT_OK(err);

    err = hound_free_ctx(ctx);
    XASSERT_OK(err);

    err = hound_destroy_driver(location);
    XASSERT_OK(err);

    return EXIT_SUCCESS;
}
