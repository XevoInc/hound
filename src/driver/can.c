/**
 * @file      can.c
 * @brief     CAN driver implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <asm/sockios.h>
#include <errno.h>
#include <hound/hound.h>
#include <hound/driver/can.h>
#include <hound-private/api.h>
#include <hound-private/driver.h>
#include <hound-private/driver/util.h>
#include <hound-private/error.h>
#include <hound-private/log.h>
#include <linux/can.h>
#include <linux/can/bcm.h>
#include <linux/can/raw.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define NSEC_PER_SEC ((uint64_t) 1e9)
#define US_PER_SEC ((uint64_t) 1e6)
#define FD_INVALID (-1)

struct bcm_payload {
    struct bcm_msg_head bcm;
    struct can_frame tx_frames[];
};

static struct hound_datadesc s_datadesc = {
    .name = "can-data",
    .data_id = HOUND_DEVICE_CAN,
    .period_count = 0,
    .avail_periods = NULL
};

/*
 * Turn off pedantic warning for nesting the variable-length struct payload at
 * the end of can_ctx, even though it is the last member of the struct.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
struct can_ctx {
    bool active;
    char iface[IFNAMSIZ];
    canid_t rx_can_id;
    int tx_fd;
    int rx_fd;
    hound_data_period period_ns;
    uint32_t tx_count;
    size_t payload_size;
    struct bcm_payload payload;
};
#pragma GCC diagnostic pop

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
hound_err can_init(void *data)
{
    struct can_ctx *ctx;
    struct ifreq ifr;
    hound_err err;
    int fd;
    size_t frames_size;
    struct hound_can_driver_init *init;

    if (data == NULL) {
        return HOUND_NULL_VAL;
    }
    init = data;

    if (init->tx_count == 0 || init->tx_count > 256) {
        return HOUND_INVALID_VAL;
    }

    if (init->tx_frames == NULL) {
        return HOUND_NULL_VAL;
    }

    if (strnlen(init->iface, IFNAMSIZ) == IFNAMSIZ) {
        return HOUND_INVALID_VAL;
    }

    /* Verify the interface exists and is usable. */
    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd == -1) {
        return fd;
    }
    strcpy(ifr.ifr_name, init->iface); /* NOLINT, string size already checked */
    err = ioctl(fd, SIOCGIFINDEX, &ifr);
    if (err == -1) {
        return errno;
    }
    close(fd);

    frames_size = init->tx_count*sizeof(*ctx->payload.tx_frames);
    ctx = malloc(sizeof(*ctx) + frames_size);
    if (ctx == NULL) {
        return HOUND_OOM;
    }

    ctx->payload_size = sizeof(ctx->payload) + frames_size;
    memcpy(ctx->payload.tx_frames, init->tx_frames, frames_size);

    strcpy(ctx->iface, init->iface); /* NOLINT, string size already checked */
    ctx->rx_can_id = init->rx_can_id;
    ctx->tx_count = init->tx_count;
    ctx->tx_fd = FD_INVALID;
    ctx->rx_fd = FD_INVALID;
    ctx->active = false;
    ctx->period_ns = 0;

    drv_set_ctx(ctx);

    return HOUND_OK;
}

static
hound_err can_destroy(void)
{
    struct can_ctx *ctx;

    ctx = drv_ctx();
    free(ctx);

    return HOUND_OK;
}

static
hound_err can_device_name(char *device_name)
{
    const struct can_ctx *ctx;

    XASSERT_NOT_NULL(device_name);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    strcpy(device_name, ctx->iface);

    return HOUND_OK;
}

static
hound_err can_datadesc(
    struct hound_datadesc **out,
    const char ***schemas,
    hound_data_count *count)
{
    struct hound_datadesc *desc;
    hound_err err;

    XASSERT_NOT_NULL(out);
    XASSERT_NOT_NULL(count);
    XASSERT_NOT_NULL(schemas);

    *count = 1;
    desc = drv_alloc(sizeof(*desc));
    if (desc == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    *schemas = drv_alloc(sizeof(**schemas));
    if (*schemas == NULL) {
        err = HOUND_OOM;
        goto error_alloc_schemas;
    }
    **schemas = "can.yaml";

    err = drv_deepcopy_desc(desc, &s_datadesc);
    if (err != HOUND_OK) {
        goto error_deepcopy;
    }

    *out = desc;
    goto out;

error_deepcopy:
    drv_free(schemas);
error_alloc_schemas:
    drv_free(desc);
out:
    return err;
}

static
hound_err populate_addr(int fd, struct sockaddr_can *addr, const char *iface)
{
    hound_err err;
    struct ifreq ifr;

    strcpy(ifr.ifr_name, iface); /* NOLINT, string size already checked */
    err = ioctl(fd, SIOCGIFINDEX, &ifr);
    if (err == -1) {
        return errno;
    }

    addr->can_family = AF_CAN;
    addr->can_ifindex = ifr.ifr_ifindex;

    return HOUND_OK;
}

static
hound_err make_raw_socket(struct can_ctx *ctx, int *out_fd)
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

    err = populate_addr(fd, &addr, ctx->iface);
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
void set_bcm_timers(struct bcm_msg_head *bcm, hound_data_period period_ns)
{
    bcm->count = 0;
    memset(&bcm->ival1, 0, sizeof(bcm->ival1));
    bcm->ival2.tv_sec = period_ns / NSEC_PER_SEC;
    bcm->ival2.tv_usec = (period_ns % NSEC_PER_SEC) / (NSEC_PER_SEC/US_PER_SEC);
}

static
hound_err make_bcm_socket(struct can_ctx *ctx, int *out_fd)
{
    struct sockaddr_can addr;
    struct bcm_msg_head *bcm;
    hound_err err;
    int fd;

    memset(&addr, 0, sizeof(addr));
    fd = socket(PF_CAN, SOCK_DGRAM, CAN_BCM);
    if (fd == -1) {
        *out_fd = FD_INVALID;
        err = errno;
        goto out;
    }

    err = populate_addr(fd, &addr, ctx->iface);
    if (err != HOUND_OK) {
        goto error;
    }

    err = connect(fd, (struct sockaddr *) &addr, sizeof(addr));
    if (err == -1) {
        err = errno;
        goto error;
    }

    /*
     * Setup BCM. We want to transmit the same messages forever, so we set count
     * to 0 and use only ival2. See Linux kernel
     * Documentation/networking/can.txt for more details.
     */
    bcm = &ctx->payload.bcm;
    bcm->opcode = TX_SETUP;
    bcm->flags = SETTIMER | STARTTIMER;
    set_bcm_timers(bcm, ctx->period_ns);
    bcm->nframes = ctx->tx_count;
    err = write_loop(fd, &ctx->payload, ctx->payload_size);
    if (err != HOUND_OK) {
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
hound_err set_period(struct can_ctx *ctx, hound_data_period period_ns)
{
    struct bcm_msg_head *bcm;
    hound_err err;
    bool transition;

    if (period_ns == ctx->period_ns) {
        return HOUND_OK;
    }

    /* Are we switching from RAW to BCM or vice-versa? */
    transition = ((ctx->period_ns == 0 && period_ns != 0) ||
                  (ctx->period_ns != 0 && period_ns == 0));
    ctx->period_ns = period_ns;

    if (!ctx->active) {
        return HOUND_OK;
    }

    if (transition) {
        close(ctx->tx_fd);
        if (period_ns == 0) {
            err = make_raw_socket(ctx, &ctx->tx_fd);
        }
        else {
            err = make_bcm_socket(ctx, &ctx->tx_fd);
        }
    }
    else {
        if (period_ns != 0) {
            /* We're changing the period of an existing BCM socket. */
            bcm = &ctx->payload.bcm;
            bcm->opcode = TX_SETUP;
            bcm->flags = SETTIMER;
            set_bcm_timers(bcm, ctx->period_ns);
            bcm->nframes = 0;
            err = write_loop(ctx->tx_fd, bcm, sizeof(*bcm));
        }
        else {
            /* RAW --> RAW; nothing to do. */
            err = HOUND_OK;
        }
    }

    return err;
}

static
hound_err can_setdata(const struct hound_data_rq_list *data_list)
{
    struct can_ctx *ctx;
    const struct hound_data_rq *rq;
    hound_err err;

    XASSERT_NOT_NULL(data_list);
    XASSERT_EQ(data_list->len, 1);
    XASSERT_NOT_NULL(data_list->data);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    rq = data_list->data;
    err = set_period(ctx, rq->period_ns);
    if (err != HOUND_OK) {
        return err;
    }

    return HOUND_OK;
}

static
hound_err can_parse(
    uint8_t *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *record_count)
{
    size_t count;
    const struct can_ctx *ctx;
    hound_err err;
    size_t i;
    const uint8_t *pos;
    struct hound_record *record;
    struct timeval tv;

    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);
    XASSERT_NOT_NULL(records);
    XASSERT_NOT_NULL(record_count);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    count = *bytes / sizeof(struct can_frame);
    if (count > HOUND_DRIVER_MAX_RECORDS) {
        count = HOUND_DRIVER_MAX_RECORDS;
    }
    XASSERT_EQ(*bytes % sizeof(struct can_frame), 0);

    pos = buf;
    for (i = 0; i < count; ++i) {
        record = &records[i];
        record->data = drv_alloc(sizeof(struct can_frame));
        if (record->data == NULL) {
            err = HOUND_OOM;
            goto error_drv_alloc;
        }
        memcpy(record->data, pos, sizeof(struct can_frame));
        record->size = sizeof(struct can_frame);

        /* Get the kernel-provided timestamp for our last message. */
        err = ioctl(ctx->rx_fd, SIOCGSTAMP, &tv);
        XASSERT_NEQ(err, -1);
        record->timestamp.tv_sec = tv.tv_sec;
        record->timestamp.tv_nsec = tv.tv_usec * (NSEC_PER_SEC/US_PER_SEC);

        record->data_id = HOUND_DEVICE_CAN;

        pos += sizeof(struct can_frame);
    }

    *record_count = count;
    *bytes -= count * sizeof(struct can_frame);

    return HOUND_OK;

error_drv_alloc:
    for (--i; i < count; --i) {
        drv_free(records[i].data);
    }
    return err;
}

static
hound_err can_next(hound_data_id id)
{
    const struct can_ctx *ctx;
    size_t i;
    hound_err err;

    XASSERT_EQ(id, HOUND_DEVICE_CAN);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    for (i = 0; i < ctx->tx_count; ++i) {
        err = write_loop(
            ctx->tx_fd,
            &ctx->payload.tx_frames[i],
            sizeof(*ctx->payload.tx_frames));
        if (err != HOUND_OK) {
            return err;
        }
    }

    return HOUND_OK;
}

static
hound_err can_start(int *out_fd)
{
    struct can_ctx *ctx;
    hound_err err;
    struct can_filter filter;
    int rx_fd;
    int tx_fd;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_EQ(ctx->tx_fd, FD_INVALID);
    XASSERT_EQ(ctx->rx_fd, FD_INVALID);

    /*
     * Open the receive socket first so that it doesn't miss the immediate
     * transmission that will happen if we open a BCM socket.
     */
    err = make_raw_socket(ctx, &rx_fd);
    if (err != HOUND_OK) {
        goto out;
    }

    /* Filter by the given CAN ID. CAN ID == 0 implies allow all traffic. */
    if (ctx->rx_can_id != 0) {
        filter.can_id = ctx->rx_can_id;
        filter.can_mask = CAN_SFF_MASK;
        err = setsockopt(
            rx_fd,
            SOL_CAN_RAW,
            CAN_RAW_FILTER,
            &filter,
            sizeof(filter));
        if (err != HOUND_OK) {
            goto out;
        }
    }

    if (ctx->period_ns == 0) {
        err = make_raw_socket(ctx, &tx_fd);
    }
    else {
        err = make_bcm_socket(ctx, &tx_fd);
    }
    if (err != HOUND_OK) {
        goto error_tx_fd;
    }

    ctx->tx_fd = tx_fd;
    ctx->rx_fd = rx_fd;
    *out_fd = rx_fd;
    ctx->active = true;

    err = HOUND_OK;
    goto out;

error_tx_fd:
    close(rx_fd);
out:
    return err;
}

static
hound_err can_stop(void)
{
    struct can_ctx *ctx;
    hound_err err;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_NEQ(ctx->tx_fd, FD_INVALID);
    XASSERT_NEQ(ctx->rx_fd, FD_INVALID);

    err = close(ctx->tx_fd);
    err |= close(ctx->rx_fd);
    ctx->tx_fd = FD_INVALID;
    ctx->rx_fd = FD_INVALID;
    ctx->active = false;

    return err;
}

static
hound_err can_reset(void *data)
{
    struct can_ctx *ctx;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    if (ctx->active) {
        can_stop();
    }
    can_destroy();
    can_init(data);

    return HOUND_OK;
}

static struct driver_ops can_driver = {
    .init = can_init,
    .destroy = can_destroy,
    .reset = can_reset,
    .device_name = can_device_name,
    .datadesc = can_datadesc,
    .setdata = can_setdata,
    .parse = can_parse,
    .start = can_start,
    .next = can_next,
    .stop = can_stop
};

PUBLIC_API
hound_err hound_register_can_driver(
    const char *schema_base,
    struct hound_can_driver_init *init)
{
    if (init == NULL) {
        return HOUND_NULL_VAL;
    }

    return driver_register(init->iface, &can_driver, schema_base, init);
}
