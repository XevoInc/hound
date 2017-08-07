/**
 * @file      core.c
 * @brief     Unit test for the core driver framework, using the no-op driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/error.h>
#include <hound/hound.h>
#include <hound/driver.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))

extern struct hound_io_driver nop_driver;

void data_cb(struct hound_record *rec)
{
    HOUND_ASSERT_NOT_NULL(rec);
}

static
void test_register(void)
{
    hound_err err;

    err = hound_register_io_driver(NULL, &nop_driver, NULL);
    HOUND_ASSERT_ERRCODE(err, HOUND_NULL_VAL);
    err = hound_register_io_driver("/dev/nop", NULL, NULL);
    HOUND_ASSERT_ERRCODE(err, HOUND_NULL_VAL);
    err = hound_register_io_driver("/dev/nop", &nop_driver, NULL);
    HOUND_ASSERT_OK(err);
}

static
void test_datadesc(void)
{
    hound_err err;
    const struct hound_datadesc **desc;
    size_t desc_len;

    err = hound_get_datadesc(NULL, &desc_len);
    HOUND_ASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_get_datadesc(&desc, NULL);
    HOUND_ASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_get_datadesc(&desc, &desc_len);
    HOUND_ASSERT_OK(err);

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
    rq.rq_list.len = rq_len;
    rq.rq_list.data = data_rq;
    err = hound_alloc_ctx(ctx, &rq);
    HOUND_ASSERT_ERRCODE(err, expected);
}

static
void test_alloc_ctx(struct hound_ctx **ctx)
{
    hound_err err;
    struct hound_data_rq data_rq[] = {
        { .id = HOUND_DEVICE_ACCELEROMETER, .freq = 1000 },
        { .id = HOUND_DEVICE_GYROSCOPE, .freq = 0 },
    };
    struct hound_data_rq bad_data_rq[ARRAYLEN(data_rq)];

    err = hound_alloc_ctx(ctx, NULL);
    HOUND_ASSERT_ERRCODE(err, HOUND_NULL_VAL);

    ctx_test(ctx, 0, data_cb, ARRAYLEN(data_rq), data_rq, HOUND_EMPTY_QUEUE);
    ctx_test(ctx, 5, data_cb, 0, NULL, HOUND_NO_DATA_REQUESTED);
    ctx_test(ctx, 5, data_cb, ARRAYLEN(data_rq), NULL, HOUND_NULL_VAL);
    ctx_test(NULL, 5, data_cb, ARRAYLEN(data_rq), data_rq, HOUND_NULL_VAL);
    ctx_test(ctx, 5, NULL, ARRAYLEN(data_rq), data_rq, HOUND_MISSING_CALLBACK);

    memcpy(bad_data_rq, data_rq, sizeof(data_rq));
    bad_data_rq[0].id = HOUND_DEVICE_TEMPERATURE;
    ctx_test(
        ctx,
        5,
        data_cb,
        ARRAYLEN(bad_data_rq),
        bad_data_rq,
        HOUND_DATA_ID_DOES_NOT_EXIST);

    memcpy(bad_data_rq, data_rq, sizeof(data_rq));
    bad_data_rq[0].freq = 999;
    ctx_test(
        ctx,
        5,
        data_cb,
        ARRAYLEN(bad_data_rq),
        bad_data_rq,
        HOUND_FREQUENCY_UNSUPPORTED);

    ctx_test(ctx, 5, data_cb, ARRAYLEN(data_rq), data_rq, HOUND_OK);
}

static
void test_start_ctx(struct hound_ctx *ctx)
{
    hound_err err;

    err = hound_start(NULL);
    HOUND_ASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_start(ctx);
    HOUND_ASSERT_OK(err);

    err = hound_start(ctx);
    HOUND_ASSERT_ERRCODE(err, HOUND_CTX_ALREADY_ACTIVE);
}

static
void test_stop_ctx(struct hound_ctx *ctx)
{
    hound_err err;

    err = hound_stop(NULL);
    HOUND_ASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_stop(ctx);
    HOUND_ASSERT_OK(err);

    err = hound_stop(ctx);
    HOUND_ASSERT_ERRCODE(err, HOUND_CTX_NOT_ACTIVE);
}

static
void test_free_ctx(struct hound_ctx *ctx)
{
    hound_err err;

    err = hound_free_ctx(NULL);
    HOUND_ASSERT_ERRCODE(err, HOUND_NULL_VAL);

    err = hound_free_ctx(ctx);
    HOUND_ASSERT_OK(err);
}

static
void test_unregister()
{
    hound_err err;

    err = hound_unregister_io_driver("/dev/nop");
    HOUND_ASSERT_OK(err);
}

int main(void) {
    struct hound_ctx *ctx;

    test_register();
    test_datadesc();
    test_alloc_ctx(&ctx);
    test_start_ctx(ctx);
    test_stop_ctx(ctx);
    test_free_ctx(ctx);
    test_unregister();

    return 0;
}
