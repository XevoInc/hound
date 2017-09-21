/**
 * @file      counter.c
 * @brief     Unit test for the counter driver, which tests the basic I/O
 *            subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound_private/valgrind.h>
#include <hound_test/assert.h>
#include <string.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))

extern hound_err register_counter_driver(size_t *count);
extern void counter_zero(void);

struct stats {
    size_t count;
    size_t seqno;
};

static void reset_counts(struct stats *stats)
{
    counter_zero();
    stats->count = 0;
}

void data_cb(struct hound_record *rec, void *cb_ctx)
{
    struct stats *stats;

    XASSERT_NOT_NULL(rec);
    XASSERT_NOT_NULL(cb_ctx);
    stats = cb_ctx;

    XASSERT_EQ(rec->size, sizeof(size_t));
    XASSERT_EQ(stats->seqno, rec->seqno);
    XASSERT_EQ(stats->count, *((size_t *) rec->data));

    ++stats->count;
    ++stats->seqno;
}

int main(void)
{
    struct hound_ctx *ctx;
    hound_err err;
    size_t count;
    size_t read;
    struct hound_rq rq;
    size_t samples;
    struct stats stats;
    struct hound_data_rq data_rq =
        { .id = HOUND_DEVICE_TEMPERATURE, .period_ns = 0 };

    /*
     * Valgrind substantially slows down runtime performance, so reduce the
     * sample count so that tests will still finish in a reasonable amount of
     * time.
     */
    if (RUNNING_ON_VALGRIND) {
        samples = 3;
    }
    else {
        samples = 4217;
    }

    count = 0;
    err = register_counter_driver(&count);
    XASSERT_OK(err);

    stats.seqno = 0;
    rq.queue_len = samples;
    rq.cb = data_cb;
    rq.cb_ctx = &stats;
    rq.rq_list.len = 1;
    rq.rq_list.data = &data_rq;
    hound_alloc_ctx(&ctx, &rq);
    XASSERT_OK(err);

    err = hound_start(ctx);
    XASSERT_OK(err);

    /* Do individual, sync reads. */
    reset_counts(&stats);
    for (count = 0; count < samples; ++count) {
        err = hound_read(ctx, 1);
        XASSERT_OK(err);
    }
    XASSERT_EQ(stats.count, count);

    /* Do one larger, sync read. */
    reset_counts(&stats);
    err = hound_read(ctx, samples);
    XASSERT_OK(err);
    XASSERT_EQ(stats.count, count);

    /* Do single async reads. */
    reset_counts(&stats);
    for (count = 0; count < samples; ++count) {
        err = hound_next(ctx, 1);
        XASSERT_OK(err);
    }
    count = 0;
    while (count < samples) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_async(ctx, 1, &read);
        XASSERT_OK(err);
        count += read;
    }
    XASSERT_EQ(stats.count, count);

    /* Do large async reads. */
    reset_counts(&stats);
    for (count = 0; count < samples; ++count) {
        hound_next(ctx, 1);
        XASSERT_OK(err);
    }
    count = 0;
    while (count < samples) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_async(ctx, samples, &read);
        XASSERT_OK(err);
        count += read;
    }
    XASSERT_EQ(count, samples);
    XASSERT_EQ(stats.count, count);

    /* Read all at once. */
    reset_counts(&stats);
    for (count = 0; count < samples; ++count) {
        hound_next(ctx, 1);
        XASSERT_OK(err);
    }
    count = 0;
    while (count < samples) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_all(ctx, &read);
        XASSERT_OK(err);
        count += read;
    }
    XASSERT_EQ(count, samples);
    XASSERT_EQ(stats.count, count);

    err = hound_stop(ctx);
    XASSERT_OK(err);

    hound_free_ctx(ctx);
    XASSERT_OK(err);

    err = hound_unregister_driver("/dev/counter");
    XASSERT_OK(err);

    return 0;
}
