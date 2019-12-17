/**
 * @file      sim.c
 * @brief     OBD II simulator. Reads from a SocketCAN socket and writes
 *            back realistic-seeming responses.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <hound-test/obd/sim.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <yobd/yobd.h>
#include <xlib/xassert.h>

static
int make_can_socket(const char *iface)
{
    struct sockaddr_can addr;
    struct can_filter filter;
    int fd;
    unsigned long index;
    int ret;

    memset(&addr, 0, sizeof(addr));
    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    XASSERT_NEQ(fd, -1);

    index = if_nametoindex(iface);
    XASSERT_NEQ(index, 0);

    addr.can_family = AF_CAN;
    addr.can_ifindex = index;
    ret = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
    XASSERT_NEQ(ret, -1);

    filter.can_id = YOBD_OBD_II_QUERY_ADDRESS;
    filter.can_mask = CAN_SFF_MASK;
    ret = setsockopt(
        fd,
        SOL_CAN_RAW,
        CAN_RAW_FILTER,
        &filter,
        sizeof(filter));
    XASSERT_EQ(ret, 0);

    return fd;
}

static
void fill_with_random(uint8_t *data, size_t bytes)
{
    size_t i;
    int r;

    for (i = 0; i < bytes; i += sizeof(r)) {
        r = rand();
        memcpy(data + i, &r, sizeof(r));
    }

    if (i < bytes) {
        r = rand();
        memcpy(data + i, &r, bytes - i);
    }
}

static
int can_response(int fd, struct yobd_ctx *ctx, struct can_frame *frame)
{
    uint8_t data[8];
    const struct yobd_pid_desc *desc;
    yobd_err err;
    yobd_mode mode;
    yobd_pid pid;
    struct can_frame response_frame;
    int ret;
    size_t written;

    err = yobd_parse_can_headers(ctx, frame, &mode, &pid);
    XASSERT_EQ(err, YOBD_OK);

    err = yobd_get_pid_descriptor(ctx, mode, pid, &desc);
    XASSERT_EQ(err, YOBD_OK);
    fill_with_random(data, desc->can_bytes);

    /*
     * The kernel doesn't seem to care about the padding/reserved bytes in
     * struct can_frame, but this makes valgrind happy.
     */
    memset(&response_frame, 0, sizeof(response_frame));

    err = yobd_make_can_response(
        ctx,
        mode,
        pid,
        data,
        desc->can_bytes,
        &response_frame);
    XASSERT_EQ(err, YOBD_OK);

    written = 0;
    do {
        ret = write(
            fd,
            &response_frame + written,
            sizeof(response_frame) - written);
        XASSERT_NEQ(ret, -1);
        written += ret;
    } while (written < sizeof(response_frame));

    return 0;
}

static
void event_loop(int fd, struct yobd_ctx *ctx)
{
    struct can_frame frame;
    int ret;

    while (true) {
        ret = read(fd, &frame, sizeof(frame));
        if (ret == -1) {
            if (errno != EINTR) {
                perror("read");
            }
            break;
        }

        ret = can_response(fd, ctx, &frame);
        if (ret == -1) {
            continue;
        }
    }
}

static
void cleanup(void *data)
{
    struct thread_ctx *ctx;

    XASSERT_NOT_NULL(data);
    ctx = data;

    if (ctx->yobd_ctx != NULL) {
        yobd_free_ctx(ctx->yobd_ctx);
    }
    if (ctx->fd != -1) {
        close(ctx->fd);
    }
}

void *run_sim(void *data)
{
    struct thread_ctx *ctx;
    yobd_err err;

    XASSERT_NOT_NULL(data);
    ctx = data;
    ctx->yobd_ctx = NULL;
    ctx->fd = -1;

    srand(time(NULL));

    pthread_cleanup_push(cleanup, ctx);

    err = yobd_parse_schema(ctx->schema_file, &ctx->yobd_ctx);
    XASSERT_EQ(err, YOBD_OK);

    ctx->fd = make_can_socket(ctx->iface);
    XASSERT_NEQ(ctx->fd, -1);

    /*
     * We have a socket now, so if the driver starts writing data, it will
     * buffer in the socket instead of being dropped. Thus it's time to wake up
     * the driver thread.
     */
    sem_post(&ctx->ready);

    event_loop(ctx->fd, ctx->yobd_ctx);

    pthread_cleanup_pop(true);

    return NULL;
}
