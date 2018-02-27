/**
 * @file      core.c
 * @brief     Unit test for the core driver framework, using the no-op driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound_test/assert.h>
#include <string.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define NS_PER_SEC (1e9)

extern hound_err register_nop_driver(void);

void data_cb(struct hound_record *rec, void *cb_ctx)
{
    XASSERT_NOT_NULL(rec);
    XASSERT_NULL(cb_ctx);
}

static
void test_register(void)
{
    hound_err err;

    err = register_nop_driver();
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
    err = strcmp(desc[0].name, "super-extra-accelerometer");
    XASSERT_EQ(err, 0);
    err = strcmp(desc[1].name, "oneshot-gyroscope");
    XASSERT_EQ(err, 0);

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
    err = hound_alloc_ctx(ctx, &rq);
    XASSERT_ERRCODE(err, expected);
}

static
void test_alloc_ctx(struct hound_ctx **ctx)
{
    hound_err err;
    struct hound_data_rq data_rq[] = {
        { .id = HOUND_DEVICE_ACCELEROMETER, .period_ns = NS_PER_SEC/1000 },
        { .id = HOUND_DEVICE_GYROSCOPE, .period_ns = 0 },
    };
    struct hound_data_rq bad_data_rq[ARRAYLEN(data_rq)];

    err = hound_alloc_ctx(ctx, NULL);
    XASSERT_ERRCODE(err, HOUND_NULL_VAL);

    ctx_test(ctx, 0, data_cb, ARRAYLEN(data_rq), data_rq, HOUND_EMPTY_QUEUE);
    ctx_test(ctx, 5, data_cb, 0, NULL, HOUND_NO_DATA_REQUESTED);
    ctx_test(ctx, 5, data_cb, ARRAYLEN(data_rq), NULL, HOUND_NULL_VAL);
    ctx_test(NULL, 5, data_cb, ARRAYLEN(data_rq), data_rq, HOUND_NULL_VAL);
    ctx_test(ctx, 5, NULL, ARRAYLEN(data_rq), data_rq, HOUND_MISSING_CALLBACK);

    memcpy(bad_data_rq, data_rq, sizeof(data_rq));
    bad_data_rq[0].id = HOUND_DEVICE_CAN;
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
    XASSERT_ERRCODE(err, HOUND_CTX_ALREADY_ACTIVE);
}

static
void test_stop_ctx(struct hound_ctx *ctx)
{
    hound_err err;

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

static
void test_unregister()
{
    hound_err err;

    err = hound_unregister_driver("/dev/nop");
    XASSERT_OK(err);
}

int main(void)
{
    struct hound_ctx *ctx;

    test_register();
    test_datadesc();
    test_alloc_ctx(&ctx);
    test_start_ctx(ctx);
    test_stop_ctx(ctx);
    test_free_ctx(ctx);
    test_unregister();

    return EXIT_SUCCESS;
}
