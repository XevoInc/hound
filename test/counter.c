/**
 * @file      counter.c
 * @brief     Unit test for the counter driver, which tests the basic I/O
 *            subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/error.h>
#include <hound/hound.h>
#include <hound/driver.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define SAMPLES (4217)

extern struct hound_io_driver counter_driver;
extern void counter_next(void);
extern void counter_zero(void);

static volatile size_t s_count = 0;
static volatile size_t s_seqno = 0;

static void reset_counts(void)
{
    counter_zero();
    s_count = 0;
}

void data_cb(struct hound_record *rec)
{
    HOUND_ASSERT_NOT_NULL(rec);
    HOUND_ASSERT_EQ(rec->size, sizeof(size_t));
    HOUND_ASSERT_EQ(s_seqno, rec->seqno);
    HOUND_ASSERT_EQ(s_count, *((size_t *) rec->data));

    ++s_count;
    ++s_seqno;
}

int main(void) {
    struct hound_ctx *ctx;
    hound_err err;
    size_t count;
    size_t read;
    struct hound_rq rq;
    struct hound_data_rq data_rq[] = {
        { .id = HOUND_DEVICE_TEMPERATURE, .freq = 0 },
    };

    err = hound_register_io_driver("/dev/counter", &counter_driver);
    HOUND_ASSERT_OK(err);

    rq.queue_len = SAMPLES;
    rq.cb = data_cb;
    rq.rq_list.len = ARRAYLEN(data_rq);
    rq.rq_list.data = data_rq;
    hound_alloc_ctx(&ctx, &rq);
    HOUND_ASSERT_OK(err);

    err = hound_start(ctx);
    HOUND_ASSERT_OK(err);

    /* Do individual, sync reads. */
    reset_counts();
    for (count = 0; count < SAMPLES; ++count) {
        counter_next();
        err = hound_read(ctx, 1);
        HOUND_ASSERT_OK(err);
    }
    HOUND_ASSERT_EQ(s_count, count);

    /* Do one larger, sync read. */
    reset_counts();
    for (count = 0; count < SAMPLES; ++count) {
        counter_next();
    }
    err = hound_read(ctx, count);
    HOUND_ASSERT_OK(err);
    HOUND_ASSERT_EQ(s_count, count);

    /* Do individual, async reads. */
    reset_counts();
    for (count = 0; count < SAMPLES; ++count) {
        counter_next();
    }
    count = 0;
    while (count < SAMPLES) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_async(ctx, 1, &read);
        HOUND_ASSERT_OK(err);
        count += read;
    }
    HOUND_ASSERT_EQ(s_count, count);

    /* Do large async reads. */
    reset_counts();
    for (count = 0; count < SAMPLES; ++count) {
        counter_next();
    }
    count = 0;
    while (count < SAMPLES) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_async(ctx, SAMPLES, &read);
        HOUND_ASSERT_OK(err);
        count += read;
    }
    HOUND_ASSERT_EQ(count, SAMPLES);
    HOUND_ASSERT_EQ(s_count, count);

    /* Read all at once. */
    reset_counts();
    for (count = 0; count < SAMPLES; ++count) {
        counter_next();
    }
    count = 0;
    while (count < SAMPLES) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_all(ctx, &read);
        HOUND_ASSERT_OK(err);
        count += read;
    }
    HOUND_ASSERT_EQ(count, SAMPLES);
    HOUND_ASSERT_EQ(s_count, count);

    err = hound_stop(ctx);
    HOUND_ASSERT_OK(err);

    hound_free_ctx(ctx);
    HOUND_ASSERT_OK(err);

    err = hound_unregister_io_driver("/dev/counter");
    HOUND_ASSERT_OK(err);

    return 0;
}
