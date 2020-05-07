/**
 * @file      sim.c
 * @brief     OBD II simulator. Reads from a SocketCAN socket and writes
 *            back realistic-seeming responses.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#define UNUSED __attribute__((unused))

#include <errno.h>
#include <fcntl.h>
#include <linux/can/raw.h>
#include <linux/if.h>
#include <net/if.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <yobd/yobd.h>
#include <xlib/xassert.h>

struct yobd_ctx *s_ctx = NULL;
int s_fd = -1;

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
void fill_with_random(unsigned char *data, size_t bytes)
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
    unsigned char data[8];
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

    srand(time(NULL));
    while (true) {
        ret = read(fd, &frame, sizeof(frame));
        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            else {
                perror("read");
                exit(EXIT_FAILURE);
            }
        }

        ret = can_response(fd, ctx, &frame);
        if (ret == -1) {
            continue;
        }
    }
}

static
void cleanup(UNUSED int signal)
{
    /*
     * Since we don't set s_ctx and s_fd atomically, technically we have a
     * possible race condition. However, since this is intended for unit test
     * code and not to run in production, I think making protecting all this
     * with a lock would be overkill. We can always add it later if needed.
     */
    if (s_ctx != NULL) {
        yobd_free_ctx(s_ctx);
    }
    if (s_fd != -1) {
        close(s_fd);
    }

    exit(EXIT_SUCCESS);
}

int main(int argc, const char **argv)
{
    yobd_err err;
    const char *iface;
    struct sigaction sa;
    sem_t *sem;
    const char * sem_name;
    const char *yobd_schema;

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s IFACE YOBD-SCHEMA [SEMAPHORE-NAME]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (strnlen(argv[1], IFNAMSIZ) == IFNAMSIZ) {
        fprintf(stderr, "Device argument is longer than IFNAMSIZ\n");
        exit(EXIT_FAILURE);
    }
    iface = argv[1];

    yobd_schema = argv[2];

    if (argc > 3) {
        sem_name = argv[3];
        sem = sem_open(sem_name, 0);
        if (sem == SEM_FAILED) {
            fprintf(
                stderr,
                "failed to open semaphore %s: %s\n",
                sem_name,
                strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
    else {
        sem = NULL;
    }

    /*
     * Register signal handlers to cleanup. Block all signals while we are
     * cleaning up, since we will exit after the cleanup anyway, and we don't
     * want to crash because of an unhandled signal.
     */
    sa.sa_handler = cleanup;
    sa.sa_flags = 0;
    sigfillset(&sa.sa_mask);

    err = sigaction(SIGHUP, &sa, NULL);
    XASSERT_EQ(err, 0);
    err = sigaction(SIGTERM, &sa, NULL);
    XASSERT_EQ(err, 0);
    err = sigaction(SIGABRT, &sa, NULL);
    XASSERT_EQ(err, 0);

    err = yobd_parse_schema(yobd_schema, &s_ctx);
    XASSERT_EQ(err, YOBD_OK);

    s_fd = make_can_socket(iface);
    XASSERT_NEQ(s_fd, -1);

    /*
     * If we were given a semaphore, signal on it to indicate we are ready to
     * respond to requests.
     */
    if (sem != NULL) {
        sem_post(sem);
        sem_close(sem);
    }

    event_loop(s_fd, s_ctx);

    /* The event loop is an infinite-loop, so we should never get here. */
    XASSERT_ERROR;
    return 0;
}
