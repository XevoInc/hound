/**
 * @file      counter.c
 * @brief     Unit test for the counter driver, which tests the basic I/O
 *            subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L

#include <hound/hound.h>
#include <hound_test/assert.h>
#include <string.h>
#include <valgrind.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))

extern hound_err register_counter_driver(size_t *count);
extern void counter_zero(void);

struct cb_ctx {
    hound_dev_id dev_id;
    size_t count;
    size_t seqno;
};

static void reset_counts(struct cb_ctx *ctx)
{
    counter_zero();
    ctx->count = 0;
}

void data_cb(const struct hound_record *rec, void *cb_ctx)
{
    const char *dev_name;
    hound_err err;
    struct cb_ctx *ctx;
    int ret;

    XASSERT_NOT_NULL(rec);
    XASSERT_NOT_NULL(cb_ctx);
    ctx = cb_ctx;

    XASSERT_EQ(rec->size, sizeof(size_t));
    XASSERT_EQ(ctx->seqno, rec->seqno);
    XASSERT_EQ(ctx->count, *((size_t *) rec->data));

    XASSERT_EQ(rec->dev_id, ctx->dev_id);

    err = hound_get_dev_name(rec->dev_id, &dev_name);
    XASSERT_OK(err);

    ret = strncmp(dev_name, "counter", HOUND_DEVICE_NAME_MAX);
    XASSERT_EQ(ret, 0);

    /* Calling with NULL should be OK (just checking if the ID is valid). */
    err = hound_get_dev_name(rec->dev_id, NULL);
    XASSERT_EQ(err, HOUND_OK);

    err = hound_get_dev_name(rec->dev_id + 1, NULL);
    XASSERT_EQ(err, HOUND_DEV_DOES_NOT_EXIST);

    ++ctx->count;
    ++ctx->seqno;
}

int main(void)
{
    size_t bytes_read;
    struct hound_ctx *ctx;
    struct hound_datadesc *desc;
    hound_err err;
    size_t count_bytes;
    size_t count_records;
    struct hound_data_rq data_rq =
        { .id = HOUND_DEVICE_GYROSCOPE, .period_ns = 0 };
    size_t len;
    size_t records_read;
    struct hound_rq rq;
    struct cb_ctx cb_ctx;
    size_t total_bytes;
    size_t total_records;

    /*
     * Valgrind substantially slows down runtime performance, so reduce the
     * sample count so that tests will still finish in a reasonable amount of
     * time.
     */
    if (RUNNING_ON_VALGRIND) {
        total_records = 3;
    }
    else {
        total_records = 4217;
    }
    total_bytes = total_records * sizeof(size_t);

    count_records = 0;
    err = register_counter_driver(&count_records);
    XASSERT_OK(err);

    cb_ctx.seqno = 0;
    rq.queue_len = total_records;
    rq.cb = data_cb;
    rq.cb_ctx = &cb_ctx;
    rq.rq_list.len = 1;
    rq.rq_list.data = &data_rq;
    hound_alloc_ctx(&ctx, &rq);
    XASSERT_OK(err);

    err = hound_start(ctx);
    XASSERT_OK(err);

    err = hound_get_datadesc(&desc, &len);
    XASSERT_OK(err);
    XASSERT_EQ(len, 1);
    cb_ctx.dev_id = desc->dev_id;

    hound_free_datadesc(desc);

    /* Do individual, sync reads. */
    reset_counts(&cb_ctx);
    for (count_records = 0; count_records < total_records; ++count_records) {
        err = hound_read(ctx, 1);
        XASSERT_OK(err);
    }
    XASSERT_EQ(cb_ctx.count, count_records);

    /* Do one larger, sync read. */
    reset_counts(&cb_ctx);
    err = hound_read(ctx, total_records);
    XASSERT_OK(err);
    XASSERT_EQ(cb_ctx.count, count_records);

    /* Do single async reads. */
    reset_counts(&cb_ctx);
    err = hound_next(ctx, total_records);
    XASSERT_OK(err);
    count_records = 0;
    while (count_records < total_records) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_nowait(ctx, 1, &records_read);
        XASSERT_OK(err);
        count_records += records_read;
    }
    XASSERT_EQ(cb_ctx.count, count_records);

    /* Do large async reads. */
    reset_counts(&cb_ctx);
    for (count_records = 0; count_records < total_records; ++count_records) {
        hound_next(ctx, 1);
        XASSERT_OK(err);
    }
    count_records = 0;
    while (count_records < total_records) {
        err = hound_read_nowait(ctx, total_records, &records_read);
        XASSERT_OK(err);
        count_records += records_read;
    }
    XASSERT_EQ(count_records, total_records);
    XASSERT_EQ(cb_ctx.count, count_records);

    /* Read all at once. */
    reset_counts(&cb_ctx);
    err = hound_next(ctx, total_records);
    XASSERT_OK(err);
    count_records = 0;
    while (count_records < total_records) {
        err = hound_read_all_nowait(ctx, &records_read);
        XASSERT_OK(err);
        count_records += records_read;
    }
    XASSERT_EQ(count_records, total_records);
    XASSERT_EQ(cb_ctx.count, count_records);

    /* Do async byte reads. */
    reset_counts(&cb_ctx);
    hound_next(ctx, total_records);
    count_bytes = 0;
    count_records = 0;
    while (count_bytes < total_bytes) {
        err = hound_read_bytes_nowait(
            ctx,
            total_bytes,
            &records_read,
            &bytes_read);
        XASSERT_OK(err);
        count_records += records_read;
        count_bytes += bytes_read;
    }
    XASSERT_EQ(count_bytes, total_bytes);
    XASSERT_EQ(count_records, total_records);
    XASSERT_EQ(cb_ctx.count, total_records);

    err = hound_stop(ctx);
    XASSERT_OK(err);

    hound_free_ctx(ctx);
    XASSERT_OK(err);

    err = hound_unregister_driver("/dev/counter");
    XASSERT_OK(err);

    return EXIT_SUCCESS;
}
