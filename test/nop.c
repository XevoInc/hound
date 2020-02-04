/**
 * @file      nop.c
 * @brief     Unit test for the core driver framework, using the no-op driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <hound/hound.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <hound-test/id.h>
#include <limits.h>
#include <linux/limits.h>
#include <string.h>

void data_cb(
    const struct hound_record *rec,
    UNUSED hound_seqno seqno,
    void *cb_ctx)
{
    XASSERT_ERROR;
    XASSERT_NOT_NULL(rec);
    XASSERT_NULL(cb_ctx);
}

static
void test_strerror(void)
{
    XASSERT_STREQ(hound_strerror(HOUND_OOM), "out of memory!");
    XASSERT_STREQ(hound_strerror(EIO), strerror(EIO));
    XASSERT_NULL(hound_strerror(INT_MIN));
}

static
void test_driver_init(const char *config_path, const char *schema_base)
{
    hound_err err;

    err = hound_init_driver("nop", "/dev/nop", schema_base, 0, NULL);
    XASSERT_OK(err);

    err = hound_destroy_driver("/dev/nop");
    XASSERT_OK(err);

    err = hound_init_config(config_path, schema_base);
    XASSERT_OK(err);

    err = hound_destroy_all_drivers();
    XASSERT_OK(err);

    err = hound_init_config(config_path, schema_base);
    XASSERT_OK(err);
}

static
void test_datadesc(void)
{
    hound_err err;
    struct hound_datadesc *desc;
    size_t desc_len;

    err = hound_get_datadesc(NULL, &desc_len);
    XASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_get_datadesc(&desc, NULL);
    XASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_get_datadesc(&desc, &desc_len);
    XASSERT_OK(err);

    XASSERT_EQ(desc[0].data_id, HOUND_DATA_NOP1);
    XASSERT_STREQ(desc[0].name, "nop");

    XASSERT_EQ(desc[0].fmt_count, 2);
    XASSERT_STREQ(desc[0].fmts[0].name, "a");
    XASSERT_EQ(desc[0].fmts[0].unit, HOUND_UNIT_PERCENT);
    XASSERT_EQ(desc[0].fmts[0].type, HOUND_TYPE_UINT8);
    XASSERT_STREQ(desc[0].fmts[1].name, "b");
    XASSERT_EQ(desc[0].fmts[1].unit, HOUND_UNIT_NONE);
    XASSERT_EQ(desc[0].fmts[1].type, HOUND_TYPE_BYTES);

    XASSERT_EQ(desc[1].data_id, HOUND_DATA_NOP2);
    XASSERT_STREQ(desc[1].name, "nop2");

    XASSERT_EQ(desc[1].fmt_count, 1);
    XASSERT_STREQ(desc[1].fmts[0].name, "x");
    XASSERT_EQ(desc[1].fmts[0].unit, HOUND_UNIT_NONE);
    XASSERT_EQ(desc[1].fmts[0].type, HOUND_TYPE_BYTES);

    hound_free_datadesc(desc);
}

static
void ctx_test(
    struct hound_ctx **ctx,
    size_t queue_len,
    hound_cb cb,
    size_t rq_len,
    struct hound_data_rq *data_rq,
    hound_err expected)
{
    hound_err err;
    struct hound_rq rq;

    rq.queue_len = queue_len;
    rq.cb = cb;
    rq.cb_ctx = NULL;
    rq.rq_list.len = rq_len;
    rq.rq_list.data = data_rq;
    err = hound_alloc_ctx(&rq, ctx);
    XASSERT_ERRCODE(err, expected);
}

static
void test_alloc_ctx(struct hound_ctx **ctx)
{
    hound_err err;
    struct hound_data_rq data_rq[] = {
        { .id = HOUND_DATA_NOP1, .period_ns = NSEC_PER_SEC/1000 },
        { .id = HOUND_DATA_NOP2, .period_ns = 0 },
    };
    struct hound_data_rq bad_data_rq[ARRAYLEN(data_rq)];

    err = hound_alloc_ctx(NULL, ctx);
    XASSERT_ERRCODE(err, HOUND_NULL_VAL);

    ctx_test(ctx, 0, data_cb, ARRAYLEN(data_rq), data_rq, HOUND_EMPTY_QUEUE);
    ctx_test(ctx, 5, data_cb, 0, NULL, HOUND_NO_DATA_REQUESTED);
    ctx_test(ctx, 5, data_cb, ARRAYLEN(data_rq), NULL, HOUND_NULL_VAL);
    ctx_test(NULL, 5, data_cb, ARRAYLEN(data_rq), data_rq, HOUND_NULL_VAL);
    ctx_test(ctx, 5, NULL, ARRAYLEN(data_rq), data_rq, HOUND_MISSING_CALLBACK);

    memcpy(bad_data_rq, data_rq, sizeof(data_rq));
    bad_data_rq[0].id = HOUND_DATA_GPS;
    ctx_test(
        ctx,
        5,
        data_cb,
        ARRAYLEN(bad_data_rq),
        bad_data_rq,
        HOUND_DATA_ID_DOES_NOT_EXIST);

    memcpy(bad_data_rq, data_rq, sizeof(data_rq));
    bad_data_rq[0].period_ns = 999;
    ctx_test(
        ctx,
        5,
        data_cb,
        ARRAYLEN(bad_data_rq),
        bad_data_rq,
        HOUND_PERIOD_UNSUPPORTED);

    ctx_test(ctx, 5, data_cb, ARRAYLEN(data_rq), data_rq, HOUND_OK);
}

static
void test_start_ctx(struct hound_ctx *ctx)
{
    hound_err err;

    err = hound_start(NULL);
    XASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_start(ctx);
    XASSERT_OK(err);

    err = hound_start(ctx);
    XASSERT_ERRCODE(err, HOUND_CTX_ACTIVE);
}

static
void test_stop_ctx(struct hound_ctx *ctx)
{
    hound_err err;

    err = hound_free_ctx(ctx);
    XASSERT_ERRCODE(err, HOUND_CTX_ACTIVE);

    err = hound_stop(NULL);
    XASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_stop(ctx);
    XASSERT_OK(err);

    err = hound_stop(ctx);
    XASSERT_ERRCODE(err, HOUND_CTX_NOT_ACTIVE);
}

static
void test_free_ctx(struct hound_ctx *ctx)
{
    hound_err err;

    err = hound_free_ctx(NULL);
    XASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_free_ctx(ctx);
    XASSERT_OK(err);
}

int main(int argc, const char **argv)
{
    const char *config_path;
    struct hound_ctx *ctx;
    const char *schema_base;

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

    test_strerror();
    test_driver_init(config_path, schema_base);
    test_datadesc();
    test_alloc_ctx(&ctx);
    test_start_ctx(ctx);
    test_stop_ctx(ctx);
    test_free_ctx(ctx);

    return EXIT_SUCCESS;
}
