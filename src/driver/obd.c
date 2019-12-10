/**
 * @file      obd.c
 * @brief     OBD-II over CAN driver implementation, using the yobd library.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <asm/sockios.h>
#include <errno.h>
#include <hound/hound.h>
#include <hound/driver/obd.h>
#include <hound-private/driver.h>
#include <hound-private/driver/util.h>
#include <hound-private/error.h>
#include <hound-private/log.h>
#include <hound-private/util.h>
#include <linux/can.h>
#include <linux/can/bcm.h>
#include <linux/can/raw.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <xlib/xvec.h>
#include <yobd/yobd.h>

#define OBD_PREFIX 0xff000000
#define MODE_MASK  0x00ff0000
#define PID_MASK   0x0000ffff

#define USEC_PER_SEC ((uint64_t) 1e6)
#define FD_INVALID (-1)

struct bcm_payload {
    struct bcm_msg_head msg_head;
    struct can_frame tx_frame;
};

struct bcm_info {
    bool active;
    struct hound_data_rq rq;
    struct bcm_payload payload;
};

struct obd_ctx {
    char iface[IFNAMSIZ];
    canid_t tx_id;
    int bcm_fd;
    int raw_fd;
    int rx_fd;
    xvec_t(struct bcm_info) bcm_rqs;
    const char *yobd_schema;
    struct yobd_ctx *yobd_ctx;
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
hound_err obd_init(void *data)
{
    struct obd_ctx *ctx;
    hound_err err;
    int fd;
    struct hound_obd_driver_init *init;
    unsigned int if_index;
    yobd_err yerr;
    struct yobd_ctx *yobd_ctx;

    if (data == NULL) {
        err = HOUND_NULL_VAL;
        goto error_null;
    }
    init = data;

    /* Verify the interface exists and is usable. */
    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd == -1) {
        err = errno;
        goto error_socket;
    }
    if_index = if_nametoindex(init->iface);
    if (if_index == 0) {
        err = errno;
        goto error_if_index;
    }
    close(fd);

    /* Verify the yobd schema file is valid. */
    yerr = yobd_parse_schema(init->yobd_schema, &yobd_ctx);
    if (yerr != YOBD_OK) {
        err = HOUND_INVALID_VAL;
        goto err_parse_schema;
    }

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        err = HOUND_OOM;
        goto error_alloc_ctx;
    }

    ctx->yobd_schema = strdup(init->yobd_schema);
    if (ctx->yobd_schema == NULL) {
        goto error_dup_schema;
    }

    strcpy(ctx->iface, init->iface);
    ctx->tx_id = YOBD_OBD_II_QUERY_ADDRESS;
    ctx->bcm_fd = FD_INVALID;
    ctx->raw_fd = FD_INVALID;
    ctx->rx_fd = FD_INVALID;
    xv_init(ctx->bcm_rqs);
    ctx->yobd_ctx = yobd_ctx;

    drv_set_ctx(ctx);
    return HOUND_OK;

error_dup_schema:
    free(ctx);
error_alloc_ctx:
    yobd_free_ctx(yobd_ctx);
err_parse_schema:
error_if_index:
error_socket:
error_null:
    return err;
}

static
hound_err obd_destroy(void)
{
    struct obd_ctx *ctx;

    ctx = drv_ctx();
    xv_destroy(ctx->bcm_rqs);
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

    strcpy(device_name, ctx->iface);

    return HOUND_OK;
}

struct iter_helper {
    size_t i;
    struct hound_datadesc *descs;
};

static
bool make_desc(
    UNUSED const struct yobd_pid_desc *pid_desc,
    yobd_mode mode,
    yobd_pid pid,
    void *data)
{
    struct hound_datadesc *desc;
    struct iter_helper *iter;

    iter = data;
    desc = &iter->descs[iter->i];

    hound_obd_get_data_id(mode, pid, &desc->data_id);
    desc->period_count = 0;
    desc->avail_periods = NULL;

    ++iter->i;

    return false;
}

static
hound_err obd_datadesc(
    size_t *desc_count,
    struct hound_datadesc **out_descs,
    char *schema,
    drv_sched_mode *mode)
{
    struct obd_ctx *ctx;
    struct hound_datadesc *descs;
    struct iter_helper iter;
    yobd_err yerr;

    XASSERT_NOT_NULL(desc_count);
    XASSERT_NOT_NULL(out_descs);
    XASSERT_NOT_NULL(schema);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    strcpy(schema, ctx->yobd_schema);
    *mode = DRV_SCHED_PUSH;

    yerr = yobd_get_pid_count(ctx->yobd_ctx, desc_count);
    XASSERT_EQ(yerr, YOBD_OK);

    descs = drv_alloc(*desc_count * sizeof(*descs));
    if (descs == NULL && *desc_count > 0) {
        return HOUND_OOM;
    }

    iter.i = 0;
    iter.descs = descs;
    yerr = yobd_pid_foreach(ctx->yobd_ctx, make_desc, &iter);
    XASSERT_EQ(yerr, YOBD_OK);
    XASSERT_EQ(iter.i, *desc_count);

    *out_descs = descs;

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
hound_err make_raw_socket(struct obd_ctx *ctx, int *out_fd)
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
void set_bcm_timers(struct bcm_msg_head *bcm, hound_data_period period_ns)
{
    /*
     * We want to transmit the same messages forever, so we set count
     * to 0 and use only ival2. See Linux kernel
     * Documentation/networking/can.txt for more details.
     */

    bcm->count = 0;
    memset(&bcm->ival1, 0, sizeof(bcm->ival1));
    bcm->ival2.tv_sec = period_ns / NSEC_PER_SEC;
    bcm->ival2.tv_usec =
        (period_ns % NSEC_PER_SEC) / (NSEC_PER_SEC/USEC_PER_SEC);
}

static
hound_err make_bcm_socket(struct obd_ctx *ctx, int *out_fd)
{
    struct bcm_info *bcm_info;
    struct sockaddr_can addr;
    hound_err err;
    int fd;
    size_t i;

    memset(&addr, 0, sizeof(addr));
    fd = socket(PF_CAN, SOCK_DGRAM, CAN_BCM);
    if (fd == -1) {
        *out_fd = FD_INVALID;
        err = errno;
        goto out;
    }

    err = populate_addr(&addr, ctx->iface);
    if (err != HOUND_OK) {
        goto error;
    }

    err = connect(fd, (struct sockaddr *) &addr, sizeof(addr));
    if (err == -1) {
        err = errno;
        goto error;
    }

    for (i = 0; i < xv_size(ctx->bcm_rqs); ++i) {
        bcm_info = &xv_A(ctx->bcm_rqs, i);
        err = write_loop(fd, &bcm_info->payload, sizeof(bcm_info->payload));
        if (err != HOUND_OK) {
            goto error;
        }
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
void populate_bcm_info(
    struct obd_ctx *ctx,
    struct bcm_info *bcm_info,
    const struct hound_data_rq *rq)
{
    yobd_mode mode;
    struct bcm_msg_head *msg_head;
    yobd_pid pid;
    yobd_err yerr;

    /* Set request information. */
    bcm_info->active = false;
    bcm_info->rq = *rq;

    /* Set payload. */
    hound_obd_get_mode_pid(rq->id, &mode, &pid);

    /*
     * Setup BCM.      */

    /*
     * We should never fail to construct a query, since the core should be
     * giving us only ID that we understand.
     */
    yerr = yobd_make_can_query(
        ctx->yobd_ctx,
        mode,
        pid,
        &bcm_info->payload.tx_frame);
    XASSERT_EQ(yerr, YOBD_OK);

    /* Set BCM header. */
    msg_head = &bcm_info->payload.msg_head;
    msg_head->opcode = TX_SETUP;
    msg_head->flags = SETTIMER | STARTTIMER;
    /* yobd will set the CAN ID here. */
    msg_head->can_id = bcm_info->payload.tx_frame.can_id;
    msg_head->nframes = 1;
    set_bcm_timers(&bcm_info->payload.msg_head, rq->period_ns);
}

static
hound_err cancel_bcm_rq(struct obd_ctx *ctx, hound_data_id id)
{
    yobd_mode mode;
    struct bcm_msg_head msg_head;
    yobd_pid pid;

    msg_head.opcode = TX_DELETE;
    msg_head.flags = 0;
    msg_head.count = 0;
    memset(&msg_head.ival1, 0, sizeof(msg_head.ival1));
    memset(&msg_head.ival2, 0, sizeof(msg_head.ival2));
    hound_obd_get_mode_pid(id, &mode, &pid);
    msg_head.can_id = id;
    msg_head.nframes = 0;

    return write_loop(ctx->bcm_fd, &msg_head, sizeof(msg_head));
}

static
hound_err obd_setdata(const struct hound_data_rq_list *rq_list)
{
    struct bcm_info *bcm_info;
    struct obd_ctx *ctx;
    hound_err err;
    size_t i;
    size_t j;
    bool match;
    const struct hound_data_rq *rq;

    XASSERT_NOT_NULL(rq_list);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    /*
     * Cancel all BCM requests that aren't in this request list.  new request
     * list.
     */
    i = 0;
    while (i < xv_size(ctx->bcm_rqs)) {
        bcm_info = &xv_A(ctx->bcm_rqs, i);
        match = false;
        for (j = 0; j < rq_list->len; ++j) {
            rq = &rq_list->data[j];
            if (bcm_info->rq.id == rq->id &&
                bcm_info->rq.period_ns == rq->period_ns) {
                match = true;
                break;
            }
        }
        if (match) {
            ++i;
        }
        else {
            /*
             * We don't increment i here because we just swapped i with the
             * end of the list and i now points to a different entry. The
             * list size is still reduced by 1, preventing an infinite loop.
             */
            if (bcm_info->active) {
                err = cancel_bcm_rq(ctx, bcm_info->rq.id);
                if (err != HOUND_OK) {
                    RM_VEC_INDEX(ctx->bcm_rqs, i);
                    return err;
                }
            }
            RM_VEC_INDEX(ctx->bcm_rqs, i);
        }
    }

    /* Create BCM info for all new periodic requests. */
    for (i = 0; i < rq_list->len; ++i) {
        rq = &rq_list->data[i];
        /* Only add a new BCM info if the request is periodic. */
        if (rq->period_ns == 0) {
            continue;
        }

        /* Only add a new BCM info if we don't already have it. */
        match = false;
        for (j = 0; j < xv_size(ctx->bcm_rqs); ++j) {
            bcm_info = &xv_A(ctx->bcm_rqs, j);
            if (bcm_info->rq.id == rq->id &&
                bcm_info->rq.period_ns == rq->period_ns) {
                match = true;
                break;
            }
        }
        if (match) {
            continue;
        }

        /* This is a new BCM request, so add it. */
        bcm_info = xv_pushp(struct bcm_info, ctx->bcm_rqs);
        if (bcm_info == NULL) {
            for (--i; i < rq_list->len; ++i) {
                (void) xv_pop(ctx->bcm_rqs);
            }
            return HOUND_OOM;
        }
        populate_bcm_info(ctx, bcm_info, rq);
    }

    return HOUND_OK;
}

static
hound_err obd_parse(
    uint8_t *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *record_count)
{
    size_t count;
    const struct obd_ctx *ctx;
    hound_err err;
    size_t i;
    yobd_mode mode;
    yobd_pid pid;
    const uint8_t *pos;
    struct hound_record *record;
    struct timeval tv;
    float val;
    yobd_err yerr;

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
        record->size = sizeof(float);
        record->data = drv_alloc(record->size);
        if (record->data == NULL) {
            err = HOUND_OOM;
            goto error_drv_alloc;
        }

        /* Get the kernel-provided timestamp for our last message. */
        err = ioctl(ctx->rx_fd, SIOCGSTAMP, &tv);
        XASSERT_NEQ(err, -1);
        record->timestamp.tv_sec = tv.tv_sec;
        record->timestamp.tv_nsec = tv.tv_usec * (NSEC_PER_SEC/USEC_PER_SEC);

        yerr = yobd_parse_can_headers(
            ctx->yobd_ctx,
            (struct can_frame *) buf,
            &mode,
            &pid);
        XASSERT_EQ(yerr, YOBD_OK);
        hound_obd_get_data_id(mode, pid, &record->data_id);

        err = yobd_parse_can_response(
            ctx->yobd_ctx,
            (struct can_frame *) buf,
            &val);
        XASSERT_OK(err);

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
hound_err obd_next(hound_data_id id)
{
    const struct obd_ctx *ctx;
    struct can_frame frame;
    hound_err err;
    yobd_mode mode;
    yobd_pid pid;
    yobd_err yerr;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    hound_obd_get_mode_pid(id, &mode, &pid);

    /*
     * If this becomes a common operation, we should implement lazy cache of CAN
     * frame responses for each hound ID. This would use extra memory, and
     * yobd_make_can_query is pretty fast, so let's do this only if next()
     * latency becomes an issue.
     */
    yerr = yobd_make_can_query(ctx->yobd_ctx, mode, pid, &frame);
    XASSERT_EQ(yerr, YOBD_OK);

    err = write_loop(ctx->raw_fd, &frame, sizeof(frame));
    if (err != HOUND_OK) {
        return err;
    }

    return HOUND_OK;
}

static
hound_err obd_start(int *out_fd)
{
    int bcm_fd;
    struct bcm_info *bcm_info;
    struct obd_ctx *ctx;
    hound_err err;
    struct can_filter filter;
    size_t i;
    int raw_fd;
    int rx_fd;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_EQ(ctx->bcm_fd, FD_INVALID);
    XASSERT_EQ(ctx->raw_fd, FD_INVALID);
    XASSERT_EQ(ctx->rx_fd, FD_INVALID);

    /*
     * Open the receive socket first so that it doesn't miss the immediate
     * transmission that will happen if we open a BCM socket.
     */
    err = make_raw_socket(ctx, &rx_fd);
    if (err != HOUND_OK) {
        goto error_rx_fd;
    }

    /*
     * Filter out our own transmissions, as responses come on a different CAN
     * ID.
     */
    filter.can_id = CAN_INV_FILTER | ctx->tx_id;
    filter.can_mask = CAN_SFF_MASK;
    err = setsockopt(
        rx_fd,
        SOL_CAN_RAW,
        CAN_RAW_FILTER,
        &filter,
        sizeof(filter));
    if (err != HOUND_OK) {
        goto error_sockopt;
    }

    err = make_raw_socket(ctx, &raw_fd);
    if (err != HOUND_OK) {
        goto error_raw_fd;
    }

    err = make_bcm_socket(ctx, &bcm_fd);
    if (err != HOUND_OK) {
        goto error_bcm_fd;
    }

    for (i = 0; i < xv_size(ctx->bcm_rqs); ++i) {
        bcm_info = &xv_A(ctx->bcm_rqs, i);
        err = write_loop(bcm_fd, &bcm_info->payload, sizeof(bcm_info->payload));
        if (err == HOUND_OK) {
            bcm_info->active = true;
        }
        else {
            hound_log_err(err, "failed to start ID %u", bcm_info->rq.id);
        }
    }

    ctx->bcm_fd = bcm_fd;
    ctx->raw_fd = raw_fd;
    ctx->rx_fd = rx_fd;
    *out_fd = rx_fd;

    err = HOUND_OK;
    goto out;

error_bcm_fd:
    close(raw_fd);
error_raw_fd:
    close(rx_fd);
error_sockopt:
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
    hound_err err3;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    XASSERT_NEQ(ctx->bcm_fd, FD_INVALID);
    XASSERT_NEQ(ctx->raw_fd, FD_INVALID);
    XASSERT_NEQ(ctx->rx_fd, FD_INVALID);

    err = close(ctx->bcm_fd);
    if (err != HOUND_OK) {
        hound_log_err_nofmt(err, "failed to close raw FD");
    }

    err2 = close(ctx->raw_fd);
    if (err2 != HOUND_OK) {
        hound_log_err_nofmt(err2, "failed to close raw FD");
    }
    err3 = close(ctx->rx_fd);
    if (err != HOUND_OK) {
        hound_log_err_nofmt(err3, "failed to close rx FD");
    }

    ctx->bcm_fd = FD_INVALID;
    ctx->raw_fd = FD_INVALID;
    ctx->rx_fd = FD_INVALID;

    if (err != HOUND_OK) {
        return err;
    }
    else if (err2 != HOUND_OK) {
        return err2;
    }
    else if (err3 != HOUND_OK) {
        return err3;
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
    .parse = obd_parse,
    .start = obd_start,
    .next = obd_next,
    .stop = obd_stop
};

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

PUBLIC_API
hound_err hound_register_obd_driver(
    const char *schema_base,
    struct hound_obd_driver_init *init)
{
    if (init == NULL) {
        return HOUND_NULL_VAL;
    }

    return driver_register(init->iface, &obd_driver, schema_base, init);
}
