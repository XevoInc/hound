/**
 * @file      mqtt.c
 * @brief     MQTT + msgpack driver implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2020 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/log.h>
#include <hound-private/parse/schema.h>
#include <hound-private/util.h>
#include <msgpack.h>
#include <mosquitto.h>
#include <poll.h>
#include <xlib/xassert.h>
#include <xlib/xhash.h>
#include <xlib/xvec.h>

#define SET_DATA(data, obj, out_size, type, field) \
    do { \
        static_assert( \
            sizeof(type) <= sizeof(obj->via.field), \
            "sizes don't match!"); \
        *((type *) data) = (type) obj->via.field; \
        *out_size = sizeof(type); \
    } while (0);

XHASH_MAP_INIT_INT(ID_MAP, const struct schema_desc *)
XHASH_MAP_INIT_STR(TOPIC_MAP, const struct schema_desc *)
XHASH_SET_INIT_INT(ACTIVE_IDS)

typedef enum {
    /* Still waiting for the callback. */
    CB_PENDING,
    /* Callback fired, with an error. */
    CB_FAIL,
    /* Callback fired successfully. */
    CB_SUCCESS
} cb_state;

struct mqtt_ctx {
    /* Start/stop state. */
    bool active;

    /* Callback state. */
    cb_state connect_state;
    cb_state disconnect_state;
    cb_state subscribe_state;
    cb_state unsubscribe_state;

    /* MQTT connection properties. */
    char *host;
    int port;
    int keepalive;
    unsigned int timeout_ms;

    /* Mosquitto context object. */
    struct mosquitto *mosq;

    /* Map from data ID to schema. */
    xhash_t(ID_MAP) *id_map;

    /* Map from MQTT topic to schema, for faster topic lookup. */
    xhash_t(TOPIC_MAP) *topic_map;

    /* Set of active data IDs (which map to schema). */
    xhash_t(ACTIVE_IDS) *active_ids;

    /* Count of pending subscribe requests. */
    size_t pending_subscribe_count;
};

static
void on_connect(UNUSED struct mosquitto *mosq, void *data, int rc)
{
    struct mqtt_ctx *ctx;
    const char *msg;
    cb_state state;

    ctx = data;

    if (rc != 0) {
        state = CB_FAIL;
        if (rc == 1) {
            msg = "unacceptable protocol version";
        }
        else if (rc == 2) {
            msg = "identifier rejected";
        }
        else if (rc == 3) {
            msg = "broker unavailable";
        }
        else {
            msg = "unknown error";
        }
        hound_log(
            LOG_ERR,
            "failed to connect to MQTT broker at %s:%d: %d (%s)\n",
            ctx->host,
            ctx->port,
            rc,
            msg);
        return;
    }
    else {
        state = CB_SUCCESS;
    }

    ctx->connect_state = state;
}

static
void on_disconnect(UNUSED struct mosquitto *mosq, void *data, int rc)
{
    struct mqtt_ctx *ctx;
    cb_state state;

    ctx = data;

    if (rc != 0) {
        state = CB_FAIL;
        hound_log(
            LOG_ERR,
            "unexpected disconnect from MQTT broker at %s:%d: %d",
            ctx->host,
            ctx->port,
            rc);
    }
    else {
        state = CB_SUCCESS;
    }

    ctx->disconnect_state = state;
}

static
void on_log(
    UNUSED struct mosquitto *mosq,
    UNUSED void *data,
    int level,
    const char *msg)
{
    int syslog_level;

    if (level == MOSQ_LOG_INFO) {
        syslog_level = LOG_INFO;
    }
    else if (level == MOSQ_LOG_NOTICE) {
        syslog_level = LOG_NOTICE;
    }
    else if (level == MOSQ_LOG_WARNING) {
        syslog_level = LOG_WARNING;
    }
    else if (level == MOSQ_LOG_ERR) {
        syslog_level = LOG_ERR;
    }
    else if (level == MOSQ_LOG_DEBUG) {
        syslog_level = LOG_DEBUG;
    }
    else {
        /* Mosquitto must have added a log level. */
        XASSERT_ERROR;
    }

    hound_log_nofmt(syslog_level, msg);
}

static
void on_subscribe(
    UNUSED struct mosquitto *mosq,
    void *data,
    UNUSED int mid,
    int qos_count,
    UNUSED const int *granted_qos)
{
    struct mqtt_ctx *ctx;

    ctx = data;
    XASSERT_EQ(qos_count, (int) ctx->pending_subscribe_count);

    ctx->subscribe_state = CB_SUCCESS;
}

static
void on_unsubscribe(
    UNUSED struct mosquitto *mosq,
    UNUSED void *data,
    UNUSED int mid)
{
    struct mqtt_ctx *ctx;

    ctx = data;
    ctx->unsubscribe_state = CB_SUCCESS;
}

static
bool serialize_obj(
    const msgpack_object *obj,
    unsigned char *data,
    size_t *out_size,
    const struct hound_data_fmt *fmt)
{
    switch (obj->type) {
        case MSGPACK_OBJECT_BOOLEAN:
            if (fmt->type != HOUND_TYPE_BOOL) {
                return false;
            }
            SET_DATA(data, obj, out_size, bool, boolean);
            break;
        case MSGPACK_OBJECT_NEGATIVE_INTEGER:
            switch (fmt->type) {
                case HOUND_TYPE_INT8:
                    SET_DATA(data, obj, out_size, int8_t, i64);
                    break;
                case HOUND_TYPE_INT16:
                    SET_DATA(data, obj, out_size, int16_t, i64);
                    break;
                case HOUND_TYPE_INT32:
                    SET_DATA(data, obj, out_size, int32_t, i64);
                    break;
                case HOUND_TYPE_INT64:
                    SET_DATA(data, obj, out_size, int64_t, i64);
                    break;
                default:
                    return false;
            }
            break;
        case MSGPACK_OBJECT_POSITIVE_INTEGER:
            switch (fmt->type) {
                case HOUND_TYPE_UINT8:
                    SET_DATA(data, obj, out_size, uint8_t, u64);
                    break;
                case HOUND_TYPE_UINT16:
                    SET_DATA(data, obj, out_size, uint16_t, u64);
                    break;
                case HOUND_TYPE_UINT32:
                    SET_DATA(data, obj, out_size, uint32_t, u64);
                    break;
                case HOUND_TYPE_UINT64:
                    SET_DATA(data, obj, out_size, uint64_t, u64);
                    break;
                default:
                    return false;
            }
            break;
        case MSGPACK_OBJECT_FLOAT32:
            SET_DATA(data, obj, out_size, float, f64);
            break;
        case MSGPACK_OBJECT_FLOAT64:
            /* FLOAT and FLOAT64 are treated the same. */
            SET_DATA(data, obj, out_size, double, f64);
            break;
        case MSGPACK_OBJECT_ARRAY:
            /* This should have been handled in the caller. */
            XASSERT_ERROR;
        case MSGPACK_OBJECT_BIN:
        case MSGPACK_OBJECT_STR:
            memcpy(data, obj->via.bin.ptr, obj->via.bin.size);
            *out_size = obj->via.bin.size;
            break;
        /* Not supported. */
        case MSGPACK_OBJECT_NIL:
        case MSGPACK_OBJECT_MAP:
        case MSGPACK_OBJECT_EXT:
            return false;
    }

    return true;
}

static
void *alloc_data(
    size_t fmt_count,
    const struct hound_data_fmt *fmts,
    hound_record_size *out_size)
{
    size_t i;
    size_t size;

    size = 0;
    for (i = 0; i < fmt_count; ++i) {
        size += get_type_size(fmts[i].type);
    }

    *out_size = size;

    return drv_alloc(size);
}

static
bool parse_payload(
    const unsigned char *buf,
    size_t size,
    const struct schema_desc *schema,
    struct hound_record *record)
{
    size_t i;
    size_t len;
    const msgpack_object *obj;
    const msgpack_object *objects;
    size_t offset;
    msgpack_unpack_return rc;
    msgpack_unpacked result;
    size_t type_size;
    bool success;

    msgpack_unpacked_init(&result);
    offset = 0;
    rc = msgpack_unpack_next(&result, (const char *) buf, size, &offset);
    if (rc != MSGPACK_UNPACK_SUCCESS) {
        success = false;
        goto out;
    }

    if (result.data.type == MSGPACK_OBJECT_ARRAY) {
        len = result.data.via.array.size;
        objects = result.data.via.array.ptr;
    }
    else {
        len = 1;
        objects = &result.data;
    }

    if (len != schema->fmt_count) {
        success = false;
        goto out;
    }

    record->data = alloc_data(schema->fmt_count, schema->fmts, &record->size);
    if (record->data == NULL) {
        success = false;
        goto out;
    }

    offset = 0;
    for (i = 0; i < len; ++i) {
        obj = &objects[i];
        if (obj->type == MSGPACK_OBJECT_BIN ||
            obj->type == MSGPACK_OBJECT_STR) {
            /* Variable-size data; expand our record data to make room. */
            record->size += obj->via.bin.size;
            record->data = realloc(record->data, record->size);
            if (record->data == NULL) {
                success = false;
                goto error_parse;
            }
        }

        success = serialize_obj(
            obj,
            record->data + offset,
            &type_size,
            &schema->fmts[i]);
        if (!success) {
            goto error_parse;
        }
        offset += type_size;
    }

    success = true;
    goto out;

error_parse:
    drv_free(record->data);
out:
    msgpack_unpacked_destroy(&result);
    return success;
}

static
void make_record(
    const struct mosquitto_message *msg,
    const struct timespec *ts,
    const struct schema_desc *schema)
{
    struct hound_record record;
    bool success;

    success = parse_payload(msg->payload, msg->payloadlen, schema, &record);
    if (!success) {
        hound_log(
            LOG_WARNING,
            "failed to parse payload for data ID 0x%x",
            schema->data_id);
        return;
    }
    record.data_id = schema->data_id;
    record.timestamp = *ts;

    drv_push_records(&record, 1);
}

static
void on_message(
    UNUSED struct mosquitto *mosq,
    void *data,
    const struct mosquitto_message *msg)
{
    struct mqtt_ctx *ctx;
    xhiter_t iter;
    int rc;
    const struct schema_desc *schema;
    struct timespec ts;

    rc = clock_gettime(CLOCK_REALTIME, &ts);
    XASSERT_EQ(rc, 0);

    ctx = data;

    iter = xh_get(TOPIC_MAP, ctx->topic_map, msg->topic);
    if (iter == xh_end(ctx->topic_map)) {
        /*
         * We don't know anything about this topic, so we shouldn't have
         * received this message!
         */
        hound_log(
            LOG_WARNING,
            "received topic we didn't subscribe to: %s",
            msg->topic);
        return;
    }
    schema = xh_val(ctx->topic_map, iter);

    make_record(msg, &ts, schema);
}

static
hound_err do_write(struct mqtt_ctx *ctx)
{
    hound_err err;
    int rc;

    if (!mosquitto_want_write(ctx->mosq)) {
        return HOUND_OK;
    }

    rc = mosquitto_loop_write(ctx->mosq, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        hound_log(LOG_ERR, "MQTT failed to write: %d", rc);
        err = HOUND_IO_ERROR;
    }
    else {
        err = HOUND_OK;
    }

    return err;
}

static
void do_misc(struct mqtt_ctx *ctx)
{
    int rc;

    rc = mosquitto_loop_misc(ctx->mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        hound_log(LOG_ERR, "MQTT failed to do misc ops: %d", rc);
    }
}

static
int get_time_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * MSEC_PER_SEC) +
           (ts.tv_nsec / NSEC_PER_MSEC);
}

static
hound_err do_read(struct mqtt_ctx *ctx)
{
    hound_err err;
    int rc;

    rc = mosquitto_loop_read(ctx->mosq, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        hound_log(LOG_ERR, "MQTT failed to read: %d", rc);
        err = HOUND_IO_ERROR;
    }
    else {
        err = HOUND_OK;
    }

    return err;
}

static
hound_err do_poll(int fd, short events, int *timeout_ms)
{
    int before_poll_ms;
    hound_err err;
    int fds;
    struct pollfd pfd;
    int poll_time_ms;
    int remaining;

    pfd.fd = fd;
    pfd.events = events;
    remaining = *timeout_ms;
    while (true) {
        before_poll_ms = get_time_ms();
        fds = poll(&pfd, 1, remaining);
        poll_time_ms = get_time_ms() - before_poll_ms;
        remaining -= poll_time_ms;
        if (fds == -1) {
            if (errno == EINTR) {
                /* Interrupted; adjust the timeout and try again. */
                if (remaining <= 0) {
                    /* Timed out. */
                    remaining = 0;
                    err = HOUND_IO_ERROR;
                    break;
                }
                else {
                    /* Try again. */
                    continue;
                }
            }
            else {
                err = HOUND_IO_ERROR;
                hound_log_err(errno, "failed to drain fd %d", pfd.fd);
                break;
            }
        }
        if (fds == 1) {
            XASSERT_GT(pfd.revents & events, 0);
            err = HOUND_OK;
            break;
        }
        else if (fds == 0) {
            /* Timed out. */
            remaining = 0;
            err = HOUND_IO_ERROR;
            break;
        }
    }

    *timeout_ms = remaining;

    return err;
}

static
hound_err wait_for_reply_cb(struct mqtt_ctx *ctx, const cb_state *state)
{
    hound_err err;
    int timeout;

    /* Send requests. */
    timeout = ctx->timeout_ms;
    err = do_poll(mosquitto_socket(ctx->mosq), POLLOUT, &timeout);
    if (err != HOUND_OK) {
        return err;
    }

    err = do_write(ctx);
    if (err != HOUND_OK) {
        return err;
    }

    /*
     * Wait for a response to our written message, which will trigger callbacks.
     * Disable the on_message callback while we wait for our callback message,
     * as we can't create records outside of the poll loop.
     *
     * TODO: We should queue up received messages we receive during this time
     * and make records during the next poll. More ideally, we should also
     * inform the hound core to call poll ASAP.
     */
    mosquitto_message_callback_set(ctx->mosq, NULL);
    timeout = ctx->timeout_ms;
    while (true) {
        err = do_poll(mosquitto_socket(ctx->mosq), POLLIN, &timeout);
        if (err != HOUND_OK) {
           goto out;
        }

        /* Read from the socket and trigger a callback if we get a reply. */
        err = do_read(ctx);
        if (err != HOUND_OK) {
            return err;
        }

        if (*state == CB_FAIL) {
            err = HOUND_IO_ERROR;
            goto out;
        }
        else if (*state == CB_SUCCESS) {
            err = HOUND_OK;
            goto out;
        }
        else {
            /* No callback yet; keep reading. */
            XASSERT_EQ(*state, CB_PENDING);
        }
    }

out:
    mosquitto_message_callback_set(ctx->mosq, on_message);
    return HOUND_OK;
}

static
hound_err wait_for_write_cb(struct mqtt_ctx *ctx, const cb_state *state)
{
    hound_err err;
    int timeout;

    /* Send pending messages. */
    timeout = ctx->timeout_ms;
    err = do_poll(mosquitto_socket(ctx->mosq), POLLOUT, &timeout);
    if (err != HOUND_OK) {
        return err;
    }

    err = do_write(ctx);
    if (err != HOUND_OK) {
        return err;
    }

    if (*state == CB_SUCCESS) {
        /* Our callback fired! */
        err = HOUND_OK;
    }
    else {
        /* We timed out. */
        err = HOUND_IO_ERROR;
    }

    return err;
}

static
void reset_cb(cb_state *state)
{
    *state = CB_PENDING;
}

static
bool mosq_init_is_safe(void)
{
    /*
     * In mosquitto versions 1.6.10 and higher, init/cleanup became refcounted
     * and can be safely called multiple times. Prior to this version, we need
     * to assume that the application initializes mosquitto prior to
     * initializing this MQTT driver, as if we call init/cleanup ourselves, we
     * will cause memory leaks and/or corruption.
     */
    return mosquitto_lib_version(NULL, NULL, NULL) >= 1006010;
}

static
hound_err mqtt_init(
    const char *location,
    size_t arg_count,
    const struct hound_init_arg *args)
{
    xhash_t(ACTIVE_IDS) *active_ids;
    hound_err err;
    struct mqtt_ctx *ctx;
    char *host;
    xhash_t(ID_MAP) *id_map;
    int keepalive;
    struct mosquitto *mosq;
    int port;
    const char *p;
    int rc;
    unsigned int timeout_ms;
    xhash_t(TOPIC_MAP) *topic_map;

    /* Parse out the host and port from the combined string. */
    for (p = location; *p != '\0' && *p != ':'; ++p);
    if (*p == '\0') {
        /* No ':' separator; use the default port of 1883. */
        port = 1883;
    }
    else {
        errno = 0;
        port = strtol(p+1, NULL, 10);
        if (errno != 0) {
            return errno;
        }
        if (port < 0 || port > INT_MAX) {
            return HOUND_INVALID_VAL;
        }
    }

    /* Duplicate the host part of the location string. */
    host = malloc(sizeof(*host) * (p-location+1));
    if (host == NULL) {
        return HOUND_OOM;
    }
    memcpy(host, location, sizeof(*host) * (p-location));
    host[p-location] = '\0';

    if (arg_count != 2 ||
        args[0].type != HOUND_TYPE_UINT32 ||
        args[1].type != HOUND_TYPE_UINT32) {
        return HOUND_INVALID_VAL;
    }
    keepalive = args[0].data.as_uint32;
    timeout_ms = args[1].data.as_uint32;

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        err = HOUND_OOM;
        goto error_alloc_ctx;
    }

    id_map = xh_init(ID_MAP);
    if (id_map == NULL) {
        err = HOUND_OOM;
        goto error_alloc_id_map;
    }

    topic_map = xh_init(TOPIC_MAP);
    if (topic_map == NULL) {
        err = HOUND_OOM;
        goto error_alloc_topic_map;
    }

    active_ids = xh_init(ACTIVE_IDS);
    if (active_ids == NULL) {
        err = HOUND_OOM;
        goto error_alloc_active_ids;
    }

    if (mosq_init_is_safe()) {
        rc = mosquitto_lib_init();
        XASSERT_EQ(rc, MOSQ_ERR_SUCCESS);
    }

    errno = 0;
    mosq = mosquitto_new(NULL, true, ctx);
    if (mosq == NULL) {
        err = errno;
        goto error_mosq_new;
    }

    rc = mosquitto_threaded_set(mosq, true);
    if (rc != MOSQ_ERR_SUCCESS) {
        err = errno;
        goto error_mosq_set_threaded;
    }

    mosquitto_user_data_set(mosq, ctx);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_subscribe_callback_set(mosq, on_subscribe);
    mosquitto_unsubscribe_callback_set(mosq, on_unsubscribe);
    mosquitto_message_callback_set(mosq, on_message);
    mosquitto_log_callback_set(mosq, on_log);

    reset_cb(&ctx->connect_state);
    reset_cb(&ctx->disconnect_state);
    reset_cb(&ctx->subscribe_state);
    reset_cb(&ctx->unsubscribe_state);

    ctx->active = false;
    ctx->host = host;
    ctx->port = port;
    ctx->keepalive = keepalive;
    ctx->timeout_ms = timeout_ms;
    ctx->mosq = mosq;
    ctx->id_map = id_map;
    ctx->topic_map = topic_map;
    ctx->active_ids = active_ids;
    ctx->pending_subscribe_count = 0;

    drv_set_ctx(ctx);

    return HOUND_OK;

error_mosq_set_threaded:
    mosquitto_destroy(mosq);
error_mosq_new:
    if (mosq_init_is_safe()) {
        rc = mosquitto_lib_cleanup();
        XASSERT_EQ(rc, MOSQ_ERR_SUCCESS);
    }
    xh_destroy(ACTIVE_IDS, active_ids);
error_alloc_active_ids:
    xh_destroy(TOPIC_MAP, topic_map);
error_alloc_topic_map:
    xh_destroy(ID_MAP, id_map);
error_alloc_id_map:
    free(ctx);
error_alloc_ctx:
    free(host);
    return err;
}

static
hound_err mqtt_destroy(void)
{
    struct mqtt_ctx *ctx;
    xhiter_t iter;
    int rc;
    const struct schema_desc *schema;

    ctx = drv_ctx();

    mosquitto_destroy(ctx->mosq);

    if (mosq_init_is_safe()) {
        rc = mosquitto_lib_cleanup();
        XASSERT_EQ(rc, MOSQ_ERR_SUCCESS);
    }

    xh_destroy(ACTIVE_IDS, ctx->active_ids);

    /*
     * Don't destroy the keys in the topic map, since we share schema
     * descriptors between that map and the ID map, and we don't want to
     * double-free anything.
     */
    xh_destroy(TOPIC_MAP, ctx->topic_map);

    xh_iter(ctx->id_map, iter,
        schema = xh_val(ctx->id_map, iter);
        destroy_schema_desc((struct schema_desc *) schema);
        free((struct schema_desc *) schema);
    );
    xh_destroy(ID_MAP, ctx->id_map);

    free(ctx->host);
    free(ctx);

    return HOUND_OK;
}

static
hound_err mqtt_device_name(char *device_name)
{
    strcpy(device_name, "MQTT client");

    return HOUND_OK;
}

static
bool topic_has_wildcards(const char *topic)
{
    const char *c;

    c = topic;
    while (*c != '\0') {
        if (*c == '+' || *c == '*') {
            return true;
        }
        ++c;
    }

    return false;
}

static
hound_err mqtt_datadesc(size_t desc_count, struct drv_datadesc *descs)
{
    struct mqtt_ctx *ctx;
    struct drv_datadesc *desc;
    hound_err err;
    size_t i;
    xhiter_t iter;
    int ret;
    struct schema_desc *schema;

    ctx = drv_ctx();

    /*
     * Enable everything in the schema, as we can subscribe to any topics
     * listed. All periods are 0, MQTT is pub/sub and thus event-based. Also,
     * make a map from data ID to topic so we can efficiently handle
     * setdata() calls.
     */
    for (i = 0; i < desc_count; ++i) {
        desc = &descs[i];

        if (topic_has_wildcards(desc->schema_desc->name)) {
            /*
             * We can't get the schema info for an entire hierarchy of topics,
             * so at least for now, we just disable wildcards and insist on
             * specific topics only.
             */
            desc->enabled = false;
            continue;
        }

        desc->enabled = true;
        desc->period_count = 1;
        desc->avail_periods = drv_alloc(sizeof(*desc->avail_periods));
        if (desc->avail_periods == NULL) {
            err = HOUND_OOM;
            goto error_loop;
        }
        desc->avail_periods[0] = 0;

        schema = drv_alloc(sizeof(*schema));
        if (schema == NULL) {
            err = HOUND_OOM;
            drv_free(schema);
            goto error_loop;
        }

        err = copy_schema_desc(desc->schema_desc, schema);
        if (err != HOUND_OK) {
            goto error_loop;
        }

        iter = xh_put(ID_MAP, ctx->id_map, schema->data_id, &ret);
        if (ret == -1) {
            destroy_schema_desc(schema);
            err = HOUND_OOM;
            goto error_loop;
        }
        xh_val(ctx->id_map, iter) = schema;

        iter = xh_put(TOPIC_MAP, ctx->topic_map, schema->name, &ret);
        if (ret == -1) {
            err = HOUND_OOM;
            goto error_loop;
        }
        xh_val(ctx->topic_map, iter) = schema;
    }

    return HOUND_OK;

error_loop:
    for (--i; i < desc_count; --i) {
        drv_free(descs[i].avail_periods);
    }
    xh_iter(ctx->id_map, iter,
        destroy_schema_desc((struct schema_desc *) xh_val(ctx->id_map, iter));
    );
    xh_clear(ID_MAP, ctx->id_map);
    xh_clear(TOPIC_MAP, ctx->topic_map);
    return err;
}

static
hound_err set_topic_list(
    struct mqtt_ctx *ctx,
    const struct hound_data_rq *rqs,
    size_t rqs_len)
{
    size_t i;
    int ret;
    const struct hound_data_rq *rq;

    /*
     * Empty the active set, then fill it up again. This may seem wasteful, but
     * it's faster than first removing the active IDs that are not in the
     * request list, since we'd have to iterate through the request list for
     * each active ID (quadratic time), even in the best case.
     */
    xh_clear(ACTIVE_IDS, ctx->active_ids);
    for (i = 0; i < rqs_len; ++i) {
        rq = &rqs[i];
        xh_put(ACTIVE_IDS, ctx->active_ids, rq->id, &ret);
        /* We emptied the set, so we shouldn't ever get a hit here. */
        XASSERT_NEQ(ret, 0);
        if (ret == -1) {
            return HOUND_OOM;
        }
    }

    return HOUND_OK;
}

static
hound_err do_connect(struct mqtt_ctx *ctx)
{
    hound_err err;
    int rc;

    /* Connect to the broker, and wait for the connection to be established. */
    reset_cb(&ctx->connect_state);
    rc = mosquitto_connect(
        ctx->mosq,
        ctx->host,
        ctx->port,
        ctx->keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        if (rc == MOSQ_ERR_INVAL) {
            return HOUND_INVALID_VAL;
        }
        else if (rc == MOSQ_ERR_ERRNO) {
            return errno;
        }
    }

    err = wait_for_reply_cb(ctx, &ctx->connect_state);
    if (err != 0) {
        hound_log_err(
            err,
            "failed to connect to MQTT broker at %s:%d",
            ctx->host,
            ctx->port);
        return err;
    }

    return HOUND_OK;
}

static
hound_err do_disconnect(struct mqtt_ctx *ctx)
{
    hound_err err;
    int rc;

    /* Connect to the broker, and wait for the connection to be established. */
    reset_cb(&ctx->disconnect_state);
    rc = mosquitto_disconnect(ctx->mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        hound_log(
            LOG_ERR,
            "failed to disconnect from MQTT broker at %s:%d (error %d)",
            ctx->host,
            ctx->port,
            rc);
    }

    err = wait_for_write_cb(ctx, &ctx->disconnect_state);
    if (err != 0) {
        hound_log_err(
            err,
            "failed to disconnect from MQTT broker at %s:%d",
            ctx->host,
            ctx->port);
        return err;
    }

    return HOUND_OK;
}

static
hound_err do_subscribe(
    struct mqtt_ctx *ctx,
    size_t len,
    const char **topics)
{
    hound_err err;
    int rc;

    if (len == 0) {
        return HOUND_OK;
    }

    reset_cb(&ctx->subscribe_state);
    ctx->pending_subscribe_count = len;
    rc = mosquitto_subscribe_multiple(
        ctx->mosq,
        NULL,
        len,
        (char * const * const) topics,
        0,
        0,
        NULL);
    if (rc != MOSQ_ERR_SUCCESS) {
        return HOUND_IO_ERROR;
    }

    err = wait_for_reply_cb(ctx, &ctx->subscribe_state);
    if (err != 0) {
        return HOUND_IO_ERROR;
    }

    ctx->pending_subscribe_count = 0;

    return HOUND_OK;
}

static
hound_err do_unsubscribe(
    struct mqtt_ctx *ctx,
    size_t len,
    const char **topics)
{
    hound_err err;
    int rc;

    if (len == 0) {
        return HOUND_OK;
    }

    reset_cb(&ctx->unsubscribe_state);
    rc = mosquitto_unsubscribe_multiple(
        ctx->mosq,
        NULL,
        len,
        (char * const * const) topics,
        NULL);
    if (rc != MOSQ_ERR_SUCCESS) {
        return HOUND_IO_ERROR;
    }

    err = wait_for_reply_cb(ctx, &ctx->unsubscribe_state);
    if (err != HOUND_OK) {
        return HOUND_IO_ERROR;
    }

    return HOUND_OK;
}

static
hound_err subscribe_new(
    struct mqtt_ctx *ctx,
    const struct hound_data_rq *rqs,
    size_t rqs_len)
{
    hound_err err;
    size_t i;
    xhiter_t iter;
    xvec_t(const char *) topics;
    const struct schema_desc *schema;
    const char **val;

    /* Subscribe to any new topics in the request list. */
    xv_init(topics);
    for (i = 0; i < rqs_len; ++i) {
        iter = xh_get(ACTIVE_IDS, ctx->active_ids, rqs[i].id);
        if (iter != xh_end(ctx->active_ids)) {
            /* We are already subscribed to this topic. */
            continue;
        }

        iter = xh_get(ID_MAP, ctx->id_map, rqs[i].id);
        XASSERT_NEQ(iter, xh_end(ctx->id_map));
        schema = xh_val(ctx->id_map, iter);

        val = xv_pushp(const char *, topics);
        if (val == NULL) {
            err = HOUND_OOM;
            goto out;
        }
        *val = schema->name;
    }

    err = do_subscribe(ctx, xv_size(topics), xv_data(topics));

out:
    xv_destroy(topics);
    return err;
}

static
hound_err unsubscribe_old(
    struct mqtt_ctx *ctx,
    const struct hound_data_rq *rqs,
    size_t rqs_len)

{
    hound_data_id id;
    hound_err err;
    size_t i;
    xhiter_t iter;
    xvec_t(const char *) topics;
    const struct schema_desc *schema;
    const char **val;

    /* Unsubscribe from any old topics that we no longer care about. */
    xv_init(topics);
    xh_foreach_key(ctx->active_ids, id,
        for (i = 0; i < rqs_len; ++i) {
            if (id == rqs[i].id) {
                break;
            }
        }
        if (i < rqs_len) {
            /*
             * This data ID is in the request list, so should keep this
             * subscription.
             */
            continue;
        }

        iter = xh_get(ID_MAP, ctx->id_map, rqs[i].id);
        XASSERT_NEQ(iter, xh_end(ctx->id_map));
        schema = xh_val(ctx->id_map, iter);

        val = xv_pushp(const char *, topics);
        if (val == NULL) {
            err = HOUND_OOM;
            goto out;
        }
        *val = schema->name;
    );

    err = do_unsubscribe(ctx, xv_size(topics), xv_data(topics));

out:
    xv_destroy(topics);
    return err;
}

static
hound_err mqtt_setdata(const struct hound_data_rq *rqs, size_t rqs_len)
{
    struct mqtt_ctx *ctx;
    hound_err err;

    ctx = drv_ctx();

    if (ctx->active) {
        err = subscribe_new(ctx, rqs, rqs_len);
        if (err != HOUND_OK) {
            return err;
        }

        err = unsubscribe_old(ctx, rqs, rqs_len);
        if (err != HOUND_OK) {
            return err;
        }
    }

    err = set_topic_list(ctx, rqs, rqs_len);
    if (err != HOUND_OK) {
        return err;
    }

    return HOUND_OK;
}

static
hound_err mqtt_poll(
    short events,
    short *out_events,
    UNUSED hound_data_period poll_time,
    bool *timeout_enabled,
    hound_data_period *timeout)
{
    struct mqtt_ctx *ctx;
    short next_events;

    ctx = drv_ctx();

    /* We also want input data. */
    next_events = POLLIN;

    if (events & POLLIN) {
        do_read(ctx);
    }

    if (events & POLLOUT) {
        /* We can write now. */
        do_write(ctx);
    }

    /* If we still want to write, request it for next time. */
    if (mosquitto_want_write(ctx->mosq)) {
        next_events |= POLLOUT;
    }

    do_misc(ctx);

    *out_events = next_events;

    /*
     * Make sure we get called at least once a second in order to perform
     * miscellaneous operations, as recommended by mosquitto's
     * mosquitto_loop_misc documentation.
     */
    *timeout_enabled = true;
    *timeout = NSEC_PER_SEC;

    return HOUND_OK;
}

static
hound_err mqtt_start(int *out_fd)
{
    struct mqtt_ctx *ctx;
    hound_data_id data_id;
    hound_err err;
    int fd;
    size_t i;
    xhiter_t iter;
    const char **topics;

    ctx = drv_ctx();

    topics = malloc(xh_size(ctx->active_ids) * sizeof(const char *));
    if (topics == NULL) {
        return HOUND_OOM;
    }

    err = do_connect(ctx);
    if (err != HOUND_OK) {
        goto out;
    }

    /* Subscribe to the topics in our list. */
    i = 0;
    xh_foreach_key(ctx->active_ids, data_id,
        iter = xh_get(ID_MAP, ctx->id_map, data_id);
        XASSERT_NEQ(iter, xh_end(ctx->id_map));
        topics[i] = xh_val(ctx->id_map, iter)->name;
        ++i;
    );

    err = do_subscribe(ctx, xh_size(ctx->active_ids), topics);
    if (err != HOUND_OK) {
        goto out;
    }

    fd = mosquitto_socket(ctx->mosq);
    if (fd == -1) {
        /* This is very strange. */
        err = HOUND_DRIVER_FAIL;
        goto out;
    }

    ctx->active = true;
    *out_fd = fd;
    err = HOUND_OK;

out:
    free(topics);
    return err;
}

static
hound_err mqtt_stop(void)
{
    struct mqtt_ctx *ctx;
    hound_data_id data_id;
    hound_err err;
    size_t i;
    xhiter_t iter;
    const char **topics;

    ctx = drv_ctx();

    topics = malloc(xh_size(ctx->active_ids) * sizeof(const char *));
    if (topics == NULL) {
        return HOUND_OOM;
    }

    /* Unsubscribe from the topics in our list. */
    i = 0;
    xh_foreach_key(ctx->active_ids, data_id,
        iter = xh_get(ID_MAP, ctx->id_map, data_id);
        XASSERT_NEQ(iter, xh_end(ctx->id_map));
        topics[i] = xh_val(ctx->id_map, iter)->name;
        ++i;
    );

    err = do_unsubscribe(ctx, xh_size(ctx->active_ids), topics);
    if (err != HOUND_OK) {
        goto out;
    }
    xh_clear(ACTIVE_IDS, ctx->active_ids);

    err = do_disconnect(ctx);
    if (err != HOUND_OK) {
        goto out;
    }

    ctx->active = false;
    err = HOUND_OK;

out:
    free(topics);
    return err;
}

static struct driver_ops mqtt_driver = {
    .init = mqtt_init,
    .destroy = mqtt_destroy,
    .device_name = mqtt_device_name,
    .datadesc = mqtt_datadesc,
    .setdata = mqtt_setdata,
    .poll = mqtt_poll,
    .start = mqtt_start,
    .next = NULL,
    .stop = mqtt_stop
};

HOUND_DRIVER_REGISTER_FUNC
static void register_mqtt_driver(void)
{
    driver_register("mqtt", &mqtt_driver);
}
