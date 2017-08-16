/**
 * @file      counter.c
 * @brief     Unit test for the counter driver, which tests the basic I/O
 *            subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/error.h>
#include <hound/hound.h>
#include <hound_private/driver.h>
#include <valgrind.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))

extern struct driver_ops counter_driver;
extern void counter_next(hound_data_id id);
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

    HOUND_ASSERT_NOT_NULL(rec);
    HOUND_ASSERT_NOT_NULL(cb_ctx);
    stats = cb_ctx;

    HOUND_ASSERT_EQ(rec->size, sizeof(size_t));
    HOUND_ASSERT_EQ(stats->seqno, rec->seqno);
    HOUND_ASSERT_EQ(stats->count, *((size_t *) rec->data));

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
    err = driver_register("/dev/counter", &counter_driver, &count);
    HOUND_ASSERT_OK(err);

    stats.seqno = 0;
    rq.queue_len = samples;
    rq.cb = data_cb;
    rq.cb_ctx = &stats;
    rq.rq_list.len = 1;
    rq.rq_list.data = &data_rq;
    hound_alloc_ctx(&ctx, &rq);
    HOUND_ASSERT_OK(err);

    err = hound_start(ctx);
    HOUND_ASSERT_OK(err);

    /* Do individual, sync reads. */
    reset_counts(&stats);
    for (count = 0; count < samples; ++count) {
		/*
		 * We don't need to call counter_next, because synchronous read does
		 * automatically does that.
		 */
        err = hound_read(ctx, 1);
        HOUND_ASSERT_OK(err);
    }
    HOUND_ASSERT_EQ(stats.count, count);

    /* Do one larger, sync read. */
    reset_counts(&stats);
    err = hound_read(ctx, count);
    HOUND_ASSERT_OK(err);
    HOUND_ASSERT_EQ(stats.count, count);

    /* Do individual, async reads. */
    reset_counts(&stats);
    for (count = 0; count < samples; ++count) {
        counter_next(data_rq.id);
    }
    count = 0;
    while (count < samples) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_async(ctx, 1, &read);
        HOUND_ASSERT_OK(err);
        count += read;
    }
    HOUND_ASSERT_EQ(stats.count, count);

    /* Do large async reads. */
    reset_counts(&stats);
    for (count = 0; count < samples; ++count) {
        counter_next(data_rq.id);
    }
    count = 0;
    while (count < samples) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_async(ctx, samples, &read);
        HOUND_ASSERT_OK(err);
        count += read;
    }
    HOUND_ASSERT_EQ(count, samples);
    HOUND_ASSERT_EQ(stats.count, count);

    /* Read all at once. */
    reset_counts(&stats);
    for (count = 0; count < samples; ++count) {
        counter_next(data_rq.id);
    }
    count = 0;
    while (count < samples) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_all(ctx, &read);
        HOUND_ASSERT_OK(err);
        count += read;
    }
    HOUND_ASSERT_EQ(count, samples);
    HOUND_ASSERT_EQ(stats.count, count);

    err = hound_stop(ctx);
    HOUND_ASSERT_OK(err);

    hound_free_ctx(ctx);
    HOUND_ASSERT_OK(err);

    err = hound_unregister_driver("/dev/counter");
    HOUND_ASSERT_OK(err);

    return 0;
}
