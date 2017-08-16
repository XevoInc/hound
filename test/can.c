/**
 * @file      file.c
 * @brief     Unit test for the file driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <hound/error.h>
#include <hound/driver/can.h>
#include <hound/hound.h>
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
        .data =  { 0x2, 0x1, 0x0c, 0, 0, 0, 0, 0 },
    },
    /* Vehicle speed query */
    {
        .can_id = 0x7df,
        .can_dlc = 8,
        .data =  { 0x2, 0x1, 0x0d, 0, 0, 0, 0, 0 },
    }
};

static struct frame_ctx s_tx = {
    .pos = 0,
    .count = ARRAYLEN(s_tx_frames),
    .frames = s_tx_frames
};

void data_cb(struct hound_record *record, void *data)
{
    struct frame_ctx *ctx;
    uint8_t *p;
    int ret;

    HOUND_ASSERT_NOT_NULL(record);
    HOUND_ASSERT_NOT_NULL(record->data);
    HOUND_ASSERT_GT(record->size, 0);
    HOUND_ASSERT_NOT_NULL(data);

    HOUND_ASSERT_EQ(record->size % sizeof(*ctx->frames), 0);

    ctx = data;
    for (p = record->data; p < record->data + record->size; p += sizeof(*ctx)) {
        ret = memcmp(p, ctx->frames + ctx->pos, sizeof(*ctx->frames));
        HOUND_ASSERT_EQ(ret, 0);
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
    HOUND_ASSERT_NEQ(fd, -1);

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
    HOUND_ASSERT_OK(err);

    err = hound_start(ctx);
    HOUND_ASSERT_OK(err);

    data_rq.period_ns = period_ns;
    if (RUNNING_ON_VALGRIND) {
        n = 2;
    }
    else {
        n = 100;
    }
    for (i = 0; i < n; ++i) {
        err = hound_read(ctx, 1);
        HOUND_ASSERT_OK(err);
    }

    err = hound_stop(ctx);
    HOUND_ASSERT_OK(err);

    err = hound_free_ctx(ctx);
    HOUND_ASSERT_OK(err);
}

int main(int argc, const char **argv)
{
    hound_err err;
    struct hound_can_driver_init init;

    if (argc != 2) {
        fprintf(stderr, "Usage: can CAN-IFACE\n");
        exit(EXIT_FAILURE);
    }
    if (strnlen(argv[1], IFNAMSIZ) == IFNAMSIZ) {
        fprintf(stderr, "File argument is longer than IFNAMSIZ\n");
        exit(EXIT_FAILURE);
    }
    strcpy(init.iface, argv[1]); /* NOLINT, string size already checked */

    init.tx_count = ARRAYLEN(s_tx_frames);
    init.tx_frames = s_tx_frames;
    init.recv_own_msg = 1;

    if (!can_iface_exists(init.iface)) {
        fprintf(
            stderr,
            "Failed to open CAN interface %s\n"
            "Run sudo meson/vcan setup to create a CAN interface",
            init.iface);
        exit(EXIT_FAILURE);
    }

    err = hound_register_can_driver(&init);
    HOUND_ASSERT_OK(err);

    /* On-demand data. */
    test_read(0);

    /* Periodic data. */
    test_read(1e9/1000);

    err = hound_unregister_driver(init.iface);
    HOUND_ASSERT_OK(err);

    return 0;
}
