/**
 * @file      can.c
 * @brief     Unit test for the file driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <hound/driver/can.h>
#include <hound/hound.h>
#include <hound_test/assert.h>
#include <linux/can.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <valgrind.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))

struct frame_ctx {
    size_t pos;
    size_t count;
    struct can_frame *frames;
};

/* These are chosen to look like common OBD II PIDs. */
static struct can_frame s_tx_frames[] = {
    /* Engine RPM query */
    {
        .can_id = 0x7df,
        .can_dlc = 8,
        .data =  { 0x2, 0x1, 0x0c, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc },
    },
    /* Vehicle speed query */
    {
        .can_id = 0x7df,
        .can_dlc = 8,
        .data =  { 0x2, 0x1, 0x0d, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc },
    }
};

static struct frame_ctx s_tx = {
    .pos = 0,
    .count = ARRAYLEN(s_tx_frames),
    .frames = s_tx_frames
};

void data_cb(const struct hound_record *record, void *data)
{
    struct frame_ctx *ctx;
    uint8_t *p;
    int ret;

    XASSERT_NOT_NULL(record);
    XASSERT_NOT_NULL(record->data);
    XASSERT_GT(record->size, 0);
    XASSERT_NOT_NULL(data);

    XASSERT_EQ(record->size % sizeof(*ctx->frames), 0);

    ctx = data;
    for (p = record->data; p < record->data + record->size; p += sizeof(*ctx)) {
        ret = memcmp(p, ctx->frames + ctx->pos, sizeof(*ctx->frames));
        XASSERT_EQ(ret, 0);
        ctx->pos = (ctx->pos+1) % ctx->count;
    }
}

bool can_iface_exists(const char *iface)
{
    struct ifreq ifr;
    int fd;
    int ret;
    bool success;

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    XASSERT_NEQ(fd, -1);

    strcpy(ifr.ifr_name, iface); /* NOLINT, string size already checked */
    ret = ioctl(fd, SIOCGIFINDEX, &ifr);
    success = (ret != -1);

    close(fd);

    return success;
}

static
void test_read(hound_data_period period_ns)
{
    struct hound_ctx *ctx;
    hound_err err;
    size_t i;
    size_t n;
    struct hound_data_rq data_rq = { .id = HOUND_DEVICE_CAN };
    struct hound_rq rq = {
        .queue_len = 100,
        .cb = data_cb,
        .cb_ctx = &s_tx,
        .rq_list.len = 1,
        .rq_list.data = &data_rq
    };

    err = hound_alloc_ctx(&ctx, &rq);
    XASSERT_OK(err);

    err = hound_start(ctx);
    XASSERT_OK(err);

    data_rq.period_ns = period_ns;
    if (RUNNING_ON_VALGRIND) {
        n = 2;
    }
    else {
        n = 100;
    }
    for (i = 0; i < n; ++i) {
        err = hound_read(ctx, 1);
        XASSERT_OK(err);
    }

    err = hound_stop(ctx);
    XASSERT_OK(err);

    err = hound_free_ctx(ctx);
    XASSERT_OK(err);
}

int main(int argc, const char **argv)
{
    hound_err err;
    struct hound_can_driver_init init;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s CAN-IFACE\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    if (strnlen(argv[1], IFNAMSIZ) == IFNAMSIZ) {
        fprintf(stderr, "Device argument is longer than IFNAMSIZ\n");
        exit(EXIT_FAILURE);
    }
    strcpy(init.iface, argv[1]); /* NOLINT, string size already checked */

    if (!can_iface_exists(init.iface)) {
        fprintf(
            stderr,
            "Failed to open CAN interface %s\n"
            "Run this command to create a CAN interface:\n"
            "sudo meson/vcan setup\n",
            init.iface);
        exit(EXIT_FAILURE);
    }

    init.tx_count = ARRAYLEN(s_tx_frames);
    init.tx_frames = s_tx_frames;
    /* Don't filter responses; we want to receive our own queries. */
    init.rx_can_id = 0;

    err = hound_register_can_driver(NULL, &init);
    XASSERT_OK(err);

    /* On-demand data. */
    test_read(0);

    /* Periodic data. */
    test_read(1e9/1000);

    err = hound_unregister_driver(init.iface);
    XASSERT_OK(err);

    return EXIT_SUCCESS;
}
