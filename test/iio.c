/**
 * @file      iio.c
 * @brief     Test for the IIO driver, intended to be run manually by humans.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <float.h>
#include <hound/driver/iio.h>
#include <hound/hound.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static
void usage(const char **argv)
{
    fprintf(stderr, "Usage: %s IIO-DEVICE BUFFER-SECONDS\n", argv[0]);
}

static volatile sig_atomic_t s_sig_pending = 0;
void sig_handler(int sig)
{
    s_sig_pending = sig;
}

static
void data_cb(const struct hound_record *record, UNUSED void *ctx)
{
    float *p;
    const char *type;
    float x;
    float y;
    float z;

    XASSERT_EQ(record->size, 3 * sizeof(float));

    if (record->data_id == HOUND_DATA_ACCEL) {
        type = "accel";
    }
    else if (record->data_id == HOUND_DATA_GYRO) {
        type = "gyro";
    }
    else {
        XASSERT_ERROR;
    }

    p = (float *) record->data;
    x = *p;
    y = *(p+1);
    z = *(p+2);

    printf("%s %ld.%.9ld %f %f %f\n",
        type,
        record->timestamp.tv_sec,
        record->timestamp.tv_nsec,
        x,
        y,
        z);
}

int main(int argc, const char **argv)
{
    struct sigaction act;
    double buf_sec;
    char *end;
    struct hound_ctx *ctx;
    struct hound_datadesc *desc;
    struct hound_iio_driver_init init;
    hound_err err;
    hound_data_period freq;
    size_t i;
    size_t iio_count;
    size_t j;
    size_t len;
    struct hound_rq rq = {
        .queue_len = 1000,
        .cb = data_cb,
        .cb_ctx = NULL,
        /* This will be filled in after we query the system. */
        .rq_list = {
            .len = 0,
            .data = NULL
        }
    };
    int status;

    if (argc != 3) {
        usage(argv);
        exit(EXIT_FAILURE);
    }

    init.dev = argv[1];
    buf_sec = strtod(argv[2], &end);
    if (end == argv[2] || fabs(buf_sec) < DBL_EPSILON) {
        usage(argv);
        exit(EXIT_FAILURE);
    }
    init.buf_ns = (__typeof__(init.buf_ns)) (NSEC_PER_SEC * buf_sec);

    if (access(init.dev, R_OK) != 0) {
        fprintf(
            stderr,
            "Device %s not accessible: %d (%s)\n",
            init.dev,
            errno,
            strerror(errno));
        exit(EXIT_FAILURE);
    }

    err = hound_init_driver("iio", init.dev, NULL, &init);
    XASSERT_OK(err);

    err = hound_get_datadesc(&desc, &len);
    XASSERT_OK(err);
    XASSERT_NOT_NULL(desc);
    XASSERT_GT(len, 0);

    printf("Found available data:\n");
    iio_count = 0;
    for (i = 0; i < len; ++i) {
        if (desc[i].data_id != HOUND_DATA_ACCEL &&
            desc[i].data_id != HOUND_DATA_GYRO) {
            continue;
        }
        ++iio_count;
        if (i != len-1) {
            printf("---\n");
        }
        printf("Hound ID: %d\n", desc[i].data_id);
        printf("Name: %s\n", desc[i].name);
        printf("Available periods:");
        for (j = 0; j < desc[i].period_count; ++j) {
            printf(" %" PRIu64, desc[i].avail_periods[j]);
        }
        printf("\nAvailable frequencies:");
        for (j = 0; j < desc[i].period_count; ++j) {
            freq = NSEC_PER_SEC / desc[i].avail_periods[j];
            printf(" %" PRIu64, freq);
        }
        printf("\n---\n");
    }

    rq.rq_list.len = iio_count;
    rq.rq_list.data = malloc(iio_count * sizeof(*rq.rq_list.data));
    if (rq.rq_list.data == NULL) {
        status = EXIT_FAILURE;
        perror("malloc");
        goto error;
    }
    for (i = 0; i < iio_count; ++i) {
        rq.rq_list.data[i].id = desc[i].data_id;
        /* Arbitrarily select the first available period, likely the slowest. */
        rq.rq_list.data[i].period_ns = desc[i].avail_periods[0];
    }
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

    free(rq.rq_list.data);

    status = EXIT_SUCCESS;
    goto out;

error:
    hound_free_datadesc(desc);
out:
    err = hound_destroy_driver(init.dev);
    XASSERT_OK(err);

    return status;
}
