/**
 * @file      obd.c
 * @brief     OBD-II over CAN driver implementation, using the yobd library.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <asm/socket.h>
#include <errno.h>
#include <hound/hound.h>
#include <hound/driver/obd.h>
#include <hound-private/driver.h>
#include <hound-private/error.h>
#include <hound-private/log.h>
#include <hound-private/util.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <xlib/xhash.h>
#include <yobd/yobd.h>

#define OBD_PREFIX 0xff000000
#define MODE_MASK  0x00ff0000
#define PID_MASK   0x0000ffff

#define USEC_PER_SEC ((uint64_t) 1e6)
#define FD_INVALID (-1)

XHASH_MAP_INIT_INT(FRAME_MAP, struct can_frame)

struct obd_ctx {
    char iface[IFNAMSIZ];
    canid_t tx_id;
    int tx_fd;
    int rx_fd;
    const char *yobd_schema;
    struct yobd_ctx *yobd_ctx;
    xhash_t(FRAME_MAP) *frame_cache;
};

static
hound_err write_loop(int fd, const void *data, size_t n)
{
    ssize_t bytes;
    size_t count;

    count = 0;
    do {
        bytes = write(fd, data, n);
        if (bytes == -1) {
            if (errno == EINTR) {
                continue;
            }
            else {
                return errno;
            }
        }
        count += bytes;
    } while (count < n);

    return HOUND_OK;
}

static
hound_err obd_init(
    const char *iface,
    size_t arg_count,
    const struct hound_init_arg *args)
{
    struct obd_ctx *ctx;
    hound_err err;
    int fd;
    unsigned int if_index;
    yobd_err yerr;
    struct yobd_ctx *yobd_ctx;
    const char *yobd_schema;

    if (strnlen(iface, IFNAMSIZ) == IFNAMSIZ) {
        err = HOUND_INVALID_VAL;
        goto error_validate;
    }

    if (args == NULL) {
        return HOUND_NULL_VAL;
    }
    if (args == NULL) {
        err = HOUND_NULL_VAL;
        goto error_validate;
    }
    if (arg_count != 1 || args->type != HOUND_TYPE_BYTES) {
        err = HOUND_INVALID_VAL;
        goto error_validate;
    }
    yobd_schema = (char *) args->data.as_bytes;

    /* Verify the interface exists and is usable. */
    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd == -1) {
        err = errno;
        goto error_socket;
    }
    if_index = if_nametoindex(iface);
    if (if_index == 0) {
        err = errno;
        goto error_if_index;
    }
    close(fd);

    /* Verify the yobd schema file is valid. */
    yerr = yobd_parse_schema(yobd_schema, &yobd_ctx);
    if (yerr != YOBD_OK) {
        err = HOUND_INVALID_VAL;
        goto err_parse_schema;
    }

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        err = HOUND_OOM;
        goto error_alloc_ctx;
    }

    ctx->yobd_schema = strdup(yobd_schema);
    if (ctx->yobd_schema == NULL) {
        goto error_dup_schema;
    }

    ctx->frame_cache = xh_init(FRAME_MAP);
    if (ctx->frame_cache == NULL) {
        goto error_xh_init;
    }

    strcpy(ctx->iface, iface);
    ctx->tx_id = YOBD_OBD_II_QUERY_ADDRESS;
    ctx->tx_fd = FD_INVALID;
    ctx->rx_fd = FD_INVALID;
    ctx->yobd_ctx = yobd_ctx;

    drv_set_ctx(ctx);
    return HOUND_OK;

error_xh_init:
    free((char *) ctx->yobd_schema);
error_dup_schema:
    free(ctx);
error_alloc_ctx:
    yobd_free_ctx(yobd_ctx);
err_parse_schema:
error_if_index:
error_socket:
error_validate:
    return err;
}

static
hound_err obd_destroy(void)
{
    struct obd_ctx *ctx;

    ctx = drv_ctx();
    xh_destroy(FRAME_MAP, ctx->frame_cache);
    yobd_free_ctx(ctx->yobd_ctx);
    free((char *) ctx->yobd_schema);
    free(ctx);

    return HOUND_OK;
}

static
hound_err obd_device_name(char *device_name)
{
    const struct obd_ctx *ctx;

    XASSERT_NOT_NULL(device_name);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_GTE(HOUND_DEVICE_NAME_MAX, IFNAMSIZ);
    strcpy(device_name, ctx->iface);

    return HOUND_OK;
}

static
hound_err obd_datadesc(size_t desc_count, struct drv_datadesc *descs)
{
    struct obd_ctx *ctx;
    struct drv_datadesc *desc;
    size_t i;
    size_t pid_count;
    yobd_err yerr;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    /*
     * The PID count and Hound descriptor count should be the same, as the Hound
     * schema should be generated from the yobd schema.
     */
    yerr = yobd_get_pid_count(ctx->yobd_ctx, &pid_count);
    XASSERT_EQ(yerr, YOBD_OK);
    XASSERT_EQ(pid_count, desc_count);

    for (i = 0; i < desc_count; ++i) {
        desc = &descs[i];
        desc->enabled = true;
        desc->period_count = 0;
        desc->avail_periods = NULL;
    }

    return HOUND_OK;
}

static
hound_err populate_addr(struct sockaddr_can *addr, const char *iface)
{
    unsigned int index;

    index = if_nametoindex(iface);
    if (index == 0) {
        return errno;
    }

    addr->can_family = AF_CAN;
    addr->can_ifindex = index;

    return HOUND_OK;
}

static
hound_err make_socket(struct obd_ctx *ctx, int *out_fd)
{
    struct sockaddr_can addr;
    hound_err err;
    int fd;

    memset(&addr, 0, sizeof(addr));
    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd == -1) {
        *out_fd = FD_INVALID;
        err = errno;
        goto out;
    }

    err = populate_addr(&addr, ctx->iface);
    if (err != HOUND_OK) {
        goto error;
    }

    err = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
    if (err == -1) {
        err = errno;
        goto error;
    }

    *out_fd = fd;
    err = HOUND_OK;
    goto out;

error:
    *out_fd = FD_INVALID;
    close(fd);
out:
    return err;
}

static
hound_err obd_setdata(const struct hound_data_rq_list *rq_list)
{
    const struct obd_ctx *ctx;
    struct can_frame *frame;
    size_t i;
    hound_data_id id;
    xhiter_t iter;
    yobd_mode mode;
    yobd_pid pid;
    int ret;
    yobd_err yerr;

    XASSERT_NOT_NULL(rq_list);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    /*
     * Populate the frame map so we can make pre-"canned" (haha) requests in
     * the next call.
     */
    for (i = 0; i < rq_list->len; ++i) {
        id = rq_list->data[i].id;
        hound_obd_get_mode_pid(id, &mode, &pid);
        iter = xh_get(FRAME_MAP, ctx->frame_cache, id);
        if (iter != xh_end(ctx->frame_cache)) {
            continue;
        }

        /* Add a new cache entry. */
        iter = xh_put(FRAME_MAP, ctx->frame_cache, id, &ret);
        if (ret == -1) {
            return HOUND_OOM;
        }
        frame = &xh_val(ctx->frame_cache, iter);

        /*
         * The kernel doesn't seem to care about the padding/reserved bytes in
         * struct can_frame, but this makes valgrind happy.
         */
        memset(frame, 0, sizeof(*frame));
        yerr = yobd_make_can_query(ctx->yobd_ctx, mode, pid, frame);
        XASSERT_EQ(yerr, YOBD_OK);
    }

    return HOUND_OK;
}

static
hound_err obd_parse(unsigned char *buf, size_t bytes)
{
    size_t count;
    const struct obd_ctx *ctx;
    hound_err err;
    size_t i;
    yobd_mode mode;
    yobd_pid pid;
    const unsigned char *pos;
    struct hound_record record;
    struct timeval tv;
    yobd_err yerr;

    XASSERT_NOT_NULL(buf);
    XASSERT_GT(bytes, 0);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);
    XASSERT_EQ(bytes % sizeof(struct can_frame), 0);

    count = bytes / sizeof(struct can_frame);
    pos = buf;
    for (i = 0; i < count; ++i) {
        record.size = sizeof(float);
        record.data = drv_alloc(record.size);
        if (record.data == NULL) {
            return HOUND_OOM;
        }

        /* Get the kernel-provided timestamp for our last message. */
        err = ioctl(ctx->rx_fd, SIOCGSTAMP, &tv);
        XASSERT_NEQ(err, -1);
        record.timestamp.tv_sec = tv.tv_sec;
        record.timestamp.tv_nsec = tv.tv_usec * (NSEC_PER_SEC/USEC_PER_SEC);

        yerr = yobd_parse_can_headers(
            ctx->yobd_ctx,
            (struct can_frame *) buf,
            &mode,
            &pid);
        XASSERT_EQ(yerr, YOBD_OK);
        hound_obd_get_data_id(mode, pid, &record.data_id);

        yerr = yobd_parse_can_response(
            ctx->yobd_ctx,
            (struct can_frame *) buf,
            (float *) record.data);
        XASSERT_EQ(yerr, YOBD_OK);

        drv_push_records(&record, 1);

        pos += sizeof(struct can_frame);
    }

    return HOUND_OK;
}

static
hound_err obd_next(hound_data_id id)
{
    const struct obd_ctx *ctx;
    struct can_frame *frame;
    hound_err err;
    xhiter_t iter;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    /*
     * Fetch the CAN frame entry from our cache. We should never have a miss, as
     * we pre-populated the cache in our setdata call.
     */
    iter = xh_get(FRAME_MAP, ctx->frame_cache, id);
    XASSERT_NEQ(iter, xh_end(ctx->frame_cache));
    frame = &xh_val(ctx->frame_cache, iter);

    err = write_loop(ctx->tx_fd, frame, sizeof(*frame));
    if (err != HOUND_OK) {
        return err;
    }

    return HOUND_OK;
}

static
hound_err obd_start(int *out_fd)
{
    canid_t can_id;
    struct obd_ctx *ctx;
    int enabled;
    hound_err err;
    size_t i;
    int tx_fd;
    int rx_fd;

    canid_t range = YOBD_OBD_II_RESPONSE_END - YOBD_OBD_II_RESPONSE_BASE;
    struct can_filter filters[range];

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_EQ(ctx->tx_fd, FD_INVALID);
    XASSERT_EQ(ctx->rx_fd, FD_INVALID);

    err = make_socket(ctx, &rx_fd);
    if (err != HOUND_OK) {
        goto error_rx_fd;
    }

    /*
     * Filter out everything except responses to our queries. See
     * https://en.wikipedia.org/wiki/OBD-II_PIDs#CAN_(11-bit)_bus_format or the
     * OBD-II standards for details.
     */
    can_id = YOBD_OBD_II_RESPONSE_BASE;
    for (i = 0; i < ARRAYLEN(filters); ++i) {
        filters[i].can_id = can_id;
        filters[i].can_mask = CAN_SFF_MASK;
        ++can_id;
    }

    err = setsockopt(
        rx_fd,
        SOL_CAN_RAW,
        CAN_RAW_FILTER,
        &filters,
        sizeof(filters));
    if (err != 0) {
        err = errno;
        goto error_sockopt;
    }

    enabled = 0;
    err = setsockopt(
        rx_fd,
        SOL_SOCKET,
        SO_TIMESTAMP,
        &enabled,
        sizeof(enabled));
    if (err != HOUND_OK) {
        err = errno;
        goto error_sockopt;
    }

    err = make_socket(ctx, &tx_fd);
    if (err != HOUND_OK) {
        goto error_tx_fd;
    }

    ctx->tx_fd = tx_fd;
    ctx->rx_fd = rx_fd;
    *out_fd = rx_fd;

    err = HOUND_OK;
    goto out;

error_tx_fd:
error_sockopt:
    close(rx_fd);
error_rx_fd:
out:
    return err;
}

static
hound_err obd_stop(void)
{
    struct obd_ctx *ctx;
    hound_err err;
    hound_err err2;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_NEQ(ctx->tx_fd, FD_INVALID);
    XASSERT_NEQ(ctx->rx_fd, FD_INVALID);

    err = close(ctx->tx_fd);
    if (err != HOUND_OK) {
        hound_log_err_nofmt(err, "failed to close tx FD");
    }

    err2 = close(ctx->rx_fd);
    if (err2 != HOUND_OK) {
        hound_log_err_nofmt(err2, "failed to close rx FD");
    }

    ctx->tx_fd = FD_INVALID;
    ctx->rx_fd = FD_INVALID;

    if (err != HOUND_OK) {
        return err;
    }
    else if (err2 != HOUND_OK) {
        return err2;
    }
    else {
        return HOUND_OK;
    }
}

static struct driver_ops obd_driver = {
    .init = obd_init,
    .destroy = obd_destroy,
    .device_name = obd_device_name,
    .datadesc = obd_datadesc,
    .setdata = obd_setdata,
    .poll = drv_default_pull,
    .parse = obd_parse,
    .start = obd_start,
    .next = obd_next,
    .stop = obd_stop
};

HOUND_DRIVER_REGISTER_FUNC
static void register_obd_driver(void)
{
    driver_register("obd", &obd_driver);
}

PUBLIC_API
void hound_obd_get_mode_pid(
    hound_data_id id,
    yobd_mode *mode,
    yobd_pid *pid)
{
    *pid = id & PID_MASK;
    *mode = (id & MODE_MASK) >> 16;
}

PUBLIC_API
void hound_obd_get_data_id(yobd_mode mode, yobd_pid pid, hound_data_id *id)
{
    *id = OBD_PREFIX | (mode << 16) | pid;
}
