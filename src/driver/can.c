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

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define NS_PER_SEC ((uint64_t) 1e9)
#define US_PER_SEC ((uint64_t) 1e6)
#define FD_INVALID (-1)

struct bcm_payload {
    struct bcm_msg_head bcm;
    struct can_frame tx_frames[];
};

static const hound_device_id_count s_device_id_count = 1;
static const char *s_device_id[] = { "can-device" };
static struct hound_drv_datadesc s_datadesc = {
    .name = "can-data",
    .id = HOUND_DEVICE_CAN,
    .period_count = 0,
    .avail_periods = NULL
};

static bool s_active;
static char s_iface[IFNAMSIZ];
static int s_recv_own_msg;
static hound_data_period s_period_ns;
static hound_alloc *s_alloc;
static int s_tx_fd;
static int s_rx_fd;
static uint32_t s_tx_count;
static size_t s_payload_size;
static struct bcm_payload *s_payload;

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

hound_err can_init(hound_alloc alloc, void *data)
{
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

    frames_size = init->tx_count*sizeof(*s_payload->tx_frames);
    s_payload_size = sizeof(*s_payload) + frames_size;
    s_payload = malloc(s_payload_size);
    if (s_payload == NULL) {
        return HOUND_OOM;
    }
    memcpy(s_payload->tx_frames, init->tx_frames, frames_size);

    strcpy(s_iface, init->iface); /* NOLINT, string size already checked */
    s_recv_own_msg = init->recv_own_msg;
    s_tx_count = init->tx_count;
    s_alloc = alloc;
    s_tx_fd = FD_INVALID;
    s_rx_fd = FD_INVALID;
    s_active = false;

    return HOUND_OK;
}

hound_err can_destroy(void)
{
    s_iface[0] = '\0';
    s_alloc = NULL;
    free(s_payload);

    return HOUND_OK;
}

hound_err can_device_ids(
    const char ***device_ids,
    hound_device_id_count *count)
{
    XASSERT_NOT_NULL(device_ids);
    XASSERT_NOT_NULL(count);

    *count = s_device_id_count;
    *device_ids = s_device_id;

    return HOUND_OK;
}

hound_err can_datadesc(
    const struct hound_drv_datadesc **desc,
    hound_data_count *count)
{
    XASSERT_NOT_NULL(desc);
    XASSERT_NOT_NULL(count);

    *count = 1;
    *desc = &s_datadesc;

    return HOUND_OK;
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
    int recv_own_msgs;

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd == -1) {
        *out_fd = FD_INVALID;
        err = errno;
        goto out;
    }

    if (s_recv_own_msg) {
        err = setsockopt(
                fd,
                SOL_CAN_RAW,
                CAN_RAW_RECV_OWN_MSGS,
                &s_recv_own_msg,
                sizeof(recv_own_msgs));
        XASSERT_EQ(err, 0);
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

hound_err can_setdata(const struct hound_drv_data_list *data_list)
{
    const struct hound_drv_data *data;
    hound_err err;

    XASSERT_NOT_NULL(data_list);
    XASSERT_GT(data_list->len, 0);
    XASSERT_NOT_NULL(data_list->data);

    if (data_list->len != 1) {
        return HOUND_DRIVER_UNSUPPORTED;
    }
    data = data_list->data;

    if (data->id != HOUND_DEVICE_CAN) {
        return HOUND_DRIVER_UNSUPPORTED;
    }

    err = set_period(data->period_ns);
    if (err != HOUND_OK) {
        return err;
    }

    return HOUND_OK;
}

hound_err can_parse(
    const uint8_t *buf,
    size_t *bytes,
    struct hound_record *record)
{
    hound_err err;
    struct timeval tv;

    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);

    if (*bytes < sizeof(struct can_frame)) {
        log_msg(LOG_ERR, "incomplete CAN frame with %lu bytes", *bytes);
        return HOUND_DRIVER_FAIL;
    }

    record->data = malloc(*bytes);
    if (record->data == NULL) {
        return HOUND_OOM;
    }
    memcpy(record->data, buf, *bytes);
    record->size = *bytes;
    *bytes = 0;

    /* Get the kernel-provided timestamp for our last message. */
    err = ioctl(s_rx_fd, SIOCGSTAMP, &tv);
    XASSERT_NEQ(err, -1);
    record->timestamp.tv_sec = tv.tv_sec;
    record->timestamp.tv_nsec = tv.tv_usec * (NS_PER_SEC/US_PER_SEC);

    record->id = HOUND_DEVICE_CAN;

    return HOUND_OK;
}

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

hound_err can_start(int *out_fd)
{
    hound_err err;
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

hound_err can_reset(hound_alloc alloc, void *data)
{
    if (s_active) {
        can_stop();
    }
    can_destroy();
    can_init(alloc, data);

    return HOUND_OK;
}

static struct driver_ops can_driver = {
    .init = can_init,
    .destroy = can_destroy,
    .reset = can_reset,
    .device_ids = can_device_ids,
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
    return driver_register(init->iface, &can_driver, init);
}