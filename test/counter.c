/**
 * @file      counter.c
 * @brief     Unit test for the counter driver, which tests the basic I/O
 *            subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L

#include <hound/hound.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <hound-test/id.h>
#include <linux/limits.h>
#include <string.h>
#include <valgrind.h>

struct cb_ctx {
    struct hound_ctx *ctx;
    hound_dev_id dev_id;
    uint64_t count;
    size_t seqno;
};

void data_cb(const struct hound_record *rec, hound_seqno seqno, void *cb_ctx)
{
    struct cb_ctx *ctx;
    const char *dev_name;
    hound_err err;
    int ret;

    XASSERT_NOT_NULL(rec);
    XASSERT_NOT_NULL(cb_ctx);
    ctx = cb_ctx;

    XASSERT_EQ(rec->size, sizeof(size_t));
    XASSERT_EQ(ctx->seqno, seqno);
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

int main(int argc, const char **argv)
{
    size_t bytes_read;
    const char *config_path;
    struct hound_datadesc *desc;
    hound_err err;
    size_t count_bytes;
    size_t count_records;
    struct hound_data_rq rq_list[] =
        {
            {.id = HOUND_DATA_COUNTER, .period_ns = NSEC_PER_SEC/10000},
            {.id = HOUND_DATA_COUNTER, .period_ns = NSEC_PER_SEC/1000}
        };
    const struct hound_data_fmt *fmt;
    size_t records_read;
    struct hound_rq rq;
    const char *schema_base;
    size_t size;
    struct cb_ctx cb_ctx;
    size_t total_bytes;
    size_t total_records;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s SCHEMA-BASE-PATH CONFIG-PATH\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    if (strnlen(argv[1], PATH_MAX) == PATH_MAX) {
        fprintf(stderr, "Schema base path is longer than PATH_MAX\n");
        exit(EXIT_FAILURE);
    }
    schema_base = argv[1];
    config_path = argv[2];

    /*
     * Valgrind substantially slows down runtime performance, so reduce the
     * sample count so that tests will still finish in a reasonable amount of
     * time.
     */
    if (RUNNING_ON_VALGRIND) {
        total_records = 3;
    }
    else {
        total_records = 100;
    }
    total_bytes = total_records * sizeof(size_t);

    err = hound_init_config(config_path, schema_base);
    XASSERT_OK(err);

    cb_ctx.count = 0;
    cb_ctx.seqno = 0;
    cb_ctx.ctx = NULL;
    rq.queue_len = 100 * total_records;
    rq.cb = data_cb;
    rq.cb_ctx = &cb_ctx;
    rq.rq_list.len = ARRAYLEN(rq_list);
    rq.rq_list.data = rq_list;
    err = hound_alloc_ctx(&rq, &cb_ctx.ctx);
    XASSERT_OK(err);
    XASSERT_NOT_NULL(cb_ctx.ctx);

    err = hound_start(cb_ctx.ctx);
    XASSERT_OK(err);

    err = hound_get_datadesc(&desc, &size);
    XASSERT_OK(err);
    XASSERT_STREQ(desc->name, "counter");
    XASSERT_EQ(size, 1);
    XASSERT_EQ(desc->fmt_count, 1);

    fmt = desc->fmts;
    XASSERT_STREQ(fmt->name, "counter");
    XASSERT_EQ(fmt->offset, 0);
    XASSERT_EQ(fmt->size, sizeof(cb_ctx.count));
    XASSERT_EQ(fmt->unit, HOUND_UNIT_NONE);
    XASSERT_EQ(fmt->type, HOUND_TYPE_UINT64);

    cb_ctx.dev_id = desc->dev_id;

    hound_free_datadesc(desc);

    /* Do individual, sync reads. */
    for (count_records = 0; count_records < total_records; ++count_records) {
        err = hound_read(cb_ctx.ctx, 1);
        XASSERT_OK(err);
    }

    /* Do one larger, sync read. */
    err = hound_read(cb_ctx.ctx, total_records);
    XASSERT_OK(err);

    /* Do single async reads. */
    count_records = 0;
    while (count_records < total_records) {
        /*
         * A tight loop like this is not efficient, but it may help stress the
         * multithreaded code.
         */
        err = hound_read_nowait(cb_ctx.ctx, 1, &records_read);
        XASSERT_OK(err);
        count_records += records_read;
    }
    XASSERT_EQ(count_records, total_records);

    /* Do large async reads. */
    count_records = 0;
    while (count_records < total_records) {
        err = hound_read_nowait(
            cb_ctx.ctx,
            total_records - count_records,
            &records_read);
        XASSERT_OK(err);
        count_records += records_read;
    }
    XASSERT_EQ(count_records, total_records);

    /* Read all at once. */
    count_records = 0;
    while (count_records < total_records) {
        err = hound_read_all_nowait(cb_ctx.ctx, &records_read);
        XASSERT_OK(err);
        count_records += records_read;
    }
    XASSERT_GTE(count_records, total_records);

    /* Do async byte reads. */
    count_bytes = 0;
    count_records = 0;
    while (count_bytes < total_bytes) {
        err = hound_read_bytes_nowait(
            cb_ctx.ctx,
            total_bytes - count_bytes,
            &records_read,
            &bytes_read);
        XASSERT_OK(err);
        count_records += records_read;
        count_bytes += bytes_read;
    }
    XASSERT_EQ(count_bytes, total_bytes)
    XASSERT_GTE(count_records, total_records);

    err = hound_stop(cb_ctx.ctx);
    XASSERT_OK(err);

    hound_free_ctx(cb_ctx.ctx);
    XASSERT_OK(err);

    err = hound_destroy_driver("/dev/counter");
    XASSERT_OK(err);

    return EXIT_SUCCESS;
}
