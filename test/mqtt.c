/**
 * @file      mqtt.c
 * @brief     Unit test for the MQTT+msgpack driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2020 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <hound/hound.h>
#include <hound/hound.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <limits.h>
#include <mosquitto.h>
#include <msgpack.h>
#include <stdlib.h>
#include <string.h>
#include <xlib/xassert.h>

#define MQTT_HOST "127.0.0.1"
#define MQTT_PORT 1883
#define MQTT_PORT_STRING "1883"
#define MQTT_LOCATION (MQTT_HOST ":" MQTT_PORT_STRING)

/*
 * Use a large keepalive so we don't break the connection if we drop into the
 * debugger.
 */
#define KEEPALIVE_SEC 1000
#define LOOP_TIMEOUT_MSEC 5000

#define CHECK_RECORD(record, type, val) \
    XASSERT_EQ(record->size, sizeof(type)); \
    XASSERT_EQ(*((type *) record->data), val);

static
void make_msg_a(msgpack_packer *packer)
{
    msgpack_pack_fix_uint64(packer, 42);
}

static
void validate_msg_a(const struct hound_record *record)
{
    CHECK_RECORD(record, uint64_t, 42);
}

static
void make_msg_b(msgpack_packer *packer)
{
    msgpack_pack_fix_int8(packer, -42);
}

static
void validate_msg_b(const struct hound_record *record)
{
    CHECK_RECORD(record, int8_t, -42);
}

static
void make_msg_c(msgpack_packer *packer)
{
    msgpack_pack_fix_uint16(packer, 42);
}

static
void validate_msg_c(const struct hound_record *record)
{
    CHECK_RECORD(record, uint16_t, 42);
}

static
void make_msg_d(msgpack_packer *packer)
{
    msgpack_pack_float(packer, 42.0);
}

static
void validate_msg_d(const struct hound_record *record)
{
    CHECK_RECORD(record, float, 42.0);
}

static
void make_msg_e(msgpack_packer *packer)
{
    msgpack_pack_double(packer, 42.0);
}

static
void validate_msg_e(const struct hound_record *record)
{
    CHECK_RECORD(record, double, 42.0);
}

static const char s_bin_buf[] = {0x4, 0x2};

static
void make_msg_f(msgpack_packer *packer)
{
    msgpack_pack_bin(packer, ARRAYLEN(s_bin_buf));
    msgpack_pack_bin_body(packer, s_bin_buf, ARRAYLEN(s_bin_buf));
}

static
void validate_msg_f(const struct hound_record *record)
{
    size_t i;

    XASSERT_EQ(record->size, ARRAYLEN(s_bin_buf));
    for (i = 0; i < ARRAYLEN(s_bin_buf); ++i) {
        XASSERT_EQ(record->data[i], s_bin_buf[i]);
        XASSERT_EQ(record->data[i], s_bin_buf[i]);
    }
}

static
void make_msg_g(msgpack_packer *packer)
{
    msgpack_pack_array(packer, 4);
    msgpack_pack_float(packer, 42.0);
    msgpack_pack_float(packer, 43.0);
    msgpack_pack_float(packer, 44.0);
    msgpack_pack_bin(packer, ARRAYLEN(s_bin_buf));
    msgpack_pack_bin_body(packer, s_bin_buf, ARRAYLEN(s_bin_buf));
}

static
void validate_msg_g(const struct hound_record *record)
{
    unsigned char *bin_base;
    float *f;
    size_t i;

    XASSERT_EQ(record->size, 3*sizeof(float) + ARRAYLEN(s_bin_buf));

    f = (float *) record->data;
    XASSERT_FLTEQ(f[0], 42.0f);
    XASSERT_FLTEQ(f[1], 43.0f);
    XASSERT_FLTEQ(f[2], 44.0f);

    bin_base = (unsigned char *) &f[3];
    for (i = 0; i < ARRAYLEN(s_bin_buf); ++i) {
        XASSERT_EQ(bin_base[i], s_bin_buf[i]);
        XASSERT_EQ(bin_base[i], s_bin_buf[i]);
    }
}

typedef void (*make_msg_func)(msgpack_packer *packer);
typedef void (*validate_msg_func)(const struct hound_record *record);

struct topic_info {
    hound_data_id data_id;
    const char *topic;
    make_msg_func msg_func;
    validate_msg_func validate_func;
};

const struct topic_info s_topic_info[] = {
    {
        .data_id = 0xfe000000,
        .topic = "a",
        .msg_func = make_msg_a,
        .validate_func = validate_msg_a
    },
    {
        .data_id = 0xfe000001,
        .topic = "b",
        .msg_func = make_msg_b,
        .validate_func = validate_msg_b
    },
    {
        .data_id = 0xfe000002,
        .topic = "c",
        .msg_func = make_msg_c,
        .validate_func = validate_msg_c
    },
    {
        .data_id = 0xfe000003,
        .topic = "d",
        .msg_func = make_msg_d,
        .validate_func = validate_msg_d
    },
    {
        .data_id = 0xfe000004,
        .topic = "e",
        .msg_func = make_msg_e,
        .validate_func = validate_msg_e
    },
    {
        .data_id = 0xfe000005,
        .topic = "f",
        .msg_func = make_msg_f,
        .validate_func = validate_msg_f
    },
    {
        .data_id = 0xfe000006,
        .topic = "g",
        .msg_func = make_msg_g,
        .validate_func = validate_msg_g
    }
};

struct test_ctx {
    size_t count;
    const struct topic_info *info;
};

struct test_ctx s_ctx = {
    .count = ARRAYLEN(s_topic_info),
    .info = s_topic_info
};

static
void data_cb(
    const struct hound_record *record,
    UNUSED hound_seqno seqno,
    UNUSED void *data)
{
    size_t i;
    struct test_ctx *ctx;
    const struct topic_info *info;

    /* Find our topic info entry. */
    ctx = data;
    for (i = 0; i < ctx->count; ++i) {
        info = &ctx->info[i];
        if (record->data_id == info->data_id) {
            break;
        }
    }
    XASSERT_LT(i, ctx->count);

    info->validate_func(record);
}

static
void publish_topic(
    struct mosquitto *mosq,
    const char *topic,
    const char *buf,
    size_t len)
{
    int rc;

    rc = mosquitto_publish(mosq, NULL, topic, len, buf, 0, false);
    XASSERT_EQ(rc, MOSQ_ERR_SUCCESS);
    rc = mosquitto_loop(mosq, LOOP_TIMEOUT_MSEC, 1);
    XASSERT_EQ(rc, MOSQ_ERR_SUCCESS);
}

static
void publish_messages(const struct test_ctx *ctx, const char *host, int port)
{
    size_t i;
    const struct topic_info *info;
    struct mosquitto *mosq;
    int rc;
    msgpack_packer packer;
    msgpack_sbuffer sbuf;

    mosq = mosquitto_new(NULL, true, NULL);
    XASSERT_NOT_NULL(mosq);

    rc = mosquitto_connect(mosq, host, port, KEEPALIVE_SEC);
    XASSERT_EQ(rc, MOSQ_ERR_SUCCESS);
    rc = mosquitto_loop(mosq, LOOP_TIMEOUT_MSEC, 1);
    XASSERT_EQ(rc, MOSQ_ERR_SUCCESS);

    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&packer, &sbuf, msgpack_sbuffer_write);
    for (i = 0; i < ctx->count; ++i) {
        info = &ctx->info[i];
        info->msg_func(&packer);
        publish_topic(mosq, info->topic, sbuf.data, sbuf.size);
        msgpack_sbuffer_clear(&sbuf);
    }
    msgpack_sbuffer_destroy(&sbuf);

    mosquitto_destroy(mosq);
}

int main(int argc, const char **argv)
{
    struct hound_ctx *ctx;
    hound_err err;
    struct hound_init_arg init[] = {
        {
            /* keepalive, seconds */
            .type = HOUND_TYPE_UINT32,
            .data.as_uint32 = KEEPALIVE_SEC
        },
        {
            /* connect/disconnect timeout, milliseconds */
            .type = HOUND_TYPE_UINT32,
            .data.as_uint32 = LOOP_TIMEOUT_MSEC
        }
    };
    struct hound_data_rq *data_rq;
    struct test_ctx test_ctx = {
        .count = ARRAYLEN(s_topic_info),
        .info = s_topic_info
    };
    struct hound_data_rq data_rqs[test_ctx.count];
    size_t i;
    int rc;
    struct hound_rq rq = {
        .queue_len = 10000,
        .cb = data_cb,
        .cb_ctx = &test_ctx,
        .rq_list.len = ARRAYLEN(data_rqs),
        .rq_list.data = data_rqs
    };
    const char *schema_base;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s SCHEMA-BASE-PATH\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (strnlen(argv[1], PATH_MAX) == PATH_MAX) {
        fprintf(stderr, "Schema base path is longer than PATH_MAX\n");
        exit(EXIT_FAILURE);
    }
    schema_base = argv[1];

    for (i = 0; i < test_ctx.count; ++i) {
        data_rq = &data_rqs[i];
        data_rq->id = test_ctx.info[i].data_id;
        data_rq->period_ns = 0;
    }

    rc = mosquitto_lib_init();
    XASSERT_EQ(rc, MOSQ_ERR_SUCCESS);

    err = hound_init_driver(
        "mqtt",
        MQTT_LOCATION,
        schema_base,
        "mqtt.yaml",
        ARRAYLEN(init),
        init);
    XASSERT_OK(err);

    err = hound_alloc_ctx(&rq, &ctx);
    XASSERT_OK(err);

    err = hound_start(ctx);
    XASSERT_OK(err);

    publish_messages(&test_ctx, MQTT_HOST, MQTT_PORT);

    err = hound_read(ctx, test_ctx.count, NULL);
    XASSERT_EQ(err, HOUND_OK);

    err = hound_stop(ctx);
    XASSERT_OK(err);

    err = hound_free_ctx(ctx);
    XASSERT_OK(err);

    err = hound_destroy_driver(MQTT_LOCATION);
    XASSERT_OK(err);

    rc = mosquitto_lib_cleanup();
    XASSERT_EQ(rc, MOSQ_ERR_SUCCESS);

    return EXIT_SUCCESS;
}
