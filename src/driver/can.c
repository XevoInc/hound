/**
 * @file      can.c
 * @brief     CAN driver implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <asm/sockios.h>
#include <errno.h>
#include <hound/hound.h>
#include <hound/driver/can.h>
#include <hound_private/api.h>
#include <hound_private/driver.h>
#include <hound_private/driver/util.h>
#include <hound_private/error.h>
#include <hound_private/log.h>
#include <linux/can.h>
#include <linux/can/bcm.h>
#include <linux/can/raw.h>
#include <linux/socket.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define NS_PER_SEC ((uint64_t) 1e9)
#define US_PER_SEC ((uint64_t) 1e6)
#define FD_INVALID (-1)

struct bcm_payload {
    struct bcm_msg_head bcm;
    struct can_frame tx_frames[];
};

static const char *s_device_id = "can-device";
static struct hound_datadesc s_datadesc = {
    .name = "can-data",
    .id = HOUND_DEVICE_CAN,
    .period_count = 0,
    .avail_periods = NULL
};

static bool s_active;
static char s_iface[IFNAMSIZ];
static canid_t s_rx_can_id;
static hound_data_period s_period_ns;
static int s_tx_fd;
static int s_rx_fd;
static uint32_t s_tx_count;
static size_t s_payload_size;
static struct bcm_payload *s_payload;

static
hound_err write_loop(int fd, void *data, size_t n)
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

    frames_size = init->tx_count*sizeof(*s_payload->tx_frames);
    s_payload_size = sizeof(*s_payload) + frames_size;
    s_payload = drv_alloc(s_payload_size);
    if (s_payload == NULL) {
        return HOUND_OOM;
    }
    memcpy(s_payload->tx_frames, init->tx_frames, frames_size);

    strcpy(s_iface, init->iface); /* NOLINT, string size already checked */
    s_rx_can_id = init->rx_can_id;
    s_tx_count = init->tx_count;
    s_tx_fd = FD_INVALID;
    s_rx_fd = FD_INVALID;
    s_active = false;

    return HOUND_OK;
}

static
hound_err can_destroy(void)
{
    s_iface[0] = '\0';
    free(s_payload);

    return HOUND_OK;
}

static
hound_err can_device_id(char *device_id)
{
    XASSERT_NOT_NULL(device_id);

    strcpy(device_id, s_device_id);

    return HOUND_OK;
}

static
hound_err can_datadesc(struct hound_datadesc **out, hound_data_count *count)
{
    struct hound_datadesc *desc;
    hound_err err;

    XASSERT_NOT_NULL(out);
    XASSERT_NOT_NULL(count);

    *count = 1;
    desc = drv_alloc(sizeof(*desc));
    if (desc == NULL) {
        return HOUND_OOM;
    }
    err = drv_deepcopy_desc(desc, &s_datadesc);
    if (err != HOUND_OK) {
        drv_free(desc);
    }

    *out = desc;
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
hound_err make_raw_socket(int *out_fd)
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

    err = populate_addr(fd, &addr, s_iface);
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
    bcm->ival2.tv_sec = period_ns / NS_PER_SEC;
    bcm->ival2.tv_usec = (period_ns % NS_PER_SEC) / (NS_PER_SEC/US_PER_SEC);
}

static
hound_err make_bcm_socket(int *out_fd)
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

    err = populate_addr(fd, &addr, s_iface);
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
    bcm = &s_payload->bcm;
    bcm->opcode = TX_SETUP;
    bcm->flags = SETTIMER | STARTTIMER;
    set_bcm_timers(bcm, s_period_ns);
    bcm->nframes = s_tx_count;
    err = write_loop(fd, s_payload, s_payload_size);
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
hound_err set_period(hound_data_period period_ns)
{
    struct bcm_msg_head *bcm;
    hound_err err;
    bool transition;

    if (period_ns == s_period_ns) {
        return HOUND_OK;
    }

    /* Are we switching from RAW to BCM or vice-versa? */
    transition = ((s_period_ns == 0 && period_ns != 0) ||
                  (s_period_ns != 0 && period_ns == 0));
    s_period_ns = period_ns;

    if (!s_active) {
        return HOUND_OK;
    }

    if (transition) {
        close(s_tx_fd);
        if (period_ns == 0) {
            err = make_raw_socket(&s_tx_fd);
        }
        else {
            err = make_bcm_socket(&s_tx_fd);
        }
    }
    else {
        if (period_ns != 0) {
            /* We're changing the period of an existing BCM socket. */
            bcm = &s_payload->bcm;
            bcm->opcode = TX_SETUP;
            bcm->flags = SETTIMER;
            set_bcm_timers(bcm, s_period_ns);
            bcm->nframes = 0;
            err = write_loop(s_tx_fd, bcm, sizeof(*bcm));
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
    const struct hound_data_rq *rq;
    hound_err err;

    XASSERT_NOT_NULL(data_list);
    XASSERT_EQ(data_list->len, 1);
    XASSERT_NOT_NULL(data_list->data);

    rq = data_list->data;
    err = set_period(rq->period_ns);
    if (err != HOUND_OK) {
        return err;
    }

    return HOUND_OK;
}

static
hound_err can_parse(
    const uint8_t *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *record_count)
{
    size_t count;
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
        err = ioctl(s_rx_fd, SIOCGSTAMP, &tv);
        XASSERT_NEQ(err, -1);
        record->timestamp.tv_sec = tv.tv_sec;
        record->timestamp.tv_nsec = tv.tv_usec * (NS_PER_SEC/US_PER_SEC);

        record->id = HOUND_DEVICE_CAN;

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
    size_t i;
    hound_err err;

    XASSERT_EQ(id, HOUND_DEVICE_CAN);

    for (i = 0; i < s_tx_count; ++i) {
        err = write_loop(
            s_tx_fd,
            &s_payload->tx_frames[i],
            sizeof(*s_payload->tx_frames));
        if (err != HOUND_OK) {
            return err;
        }
    }

    return HOUND_OK;
}

static
hound_err can_start(int *out_fd)
{
    hound_err err;
    struct can_filter filter;
    int rx_fd;
    int tx_fd;

    XASSERT_EQ(s_tx_fd, FD_INVALID);
    XASSERT_EQ(s_rx_fd, FD_INVALID);

    /*
     * Open the receive socket first so that it doesn't miss the immediate
     * transmission that will happen if we open a BCM socket.
     */
    err = make_raw_socket(&rx_fd);
    if (err != HOUND_OK) {
        goto out;
    }

    /* Filter by the given CAN ID. CAN ID == 0 implies allow all traffic. */
    if (s_rx_can_id != 0) {
        filter.can_id = s_rx_can_id;
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

    if (s_period_ns == 0) {
        err = make_raw_socket(&tx_fd);
    }
    else {
        err = make_bcm_socket(&tx_fd);
    }
    if (err != HOUND_OK) {
        goto error_tx_fd;
    }

    s_tx_fd = tx_fd;
    s_rx_fd = rx_fd;
    *out_fd = rx_fd;
    s_active = true;

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
    hound_err err;

    XASSERT_NEQ(s_tx_fd, FD_INVALID);
    XASSERT_NEQ(s_rx_fd, FD_INVALID);

    err = close(s_tx_fd);
    err |= close(s_rx_fd);
    s_tx_fd = FD_INVALID;
    s_rx_fd = FD_INVALID;
    s_active = false;

    return err;
}

static
hound_err can_reset(void *data)
{
    if (s_active) {
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
    .device_id = can_device_id,
    .datadesc = can_datadesc,
    .setdata = can_setdata,
    .parse = can_parse,
    .start = can_start,
    .next = can_next,
    .stop = can_stop
};

PUBLIC_API
hound_err hound_register_can_driver(struct hound_can_driver_init *init)
{
    if (init == NULL) {
        return HOUND_NULL_VAL;
    }

    return driver_register(init->iface, &can_driver, init);
}
