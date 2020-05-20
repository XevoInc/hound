/**
 * @file      test.c
 * @brief     Unit test for the OBD-II driver. Starts up the OBD-II simulator in
 *            order to act as an OBD-II server for the OBD-II driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <hound/driver/obd.h>
#include <hound/hound.h>
#include <hound-private/util.h>
#include <hound-test/assert.h>
#include <linux/can.h>
#include <linux/limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <valgrind.h>
#include <xlib/xassert.h>

struct test_ctx {
    size_t seqno;
    size_t count;
    struct modepid *obd_rqs;
    char iface[HOUND_DEVICE_NAME_MAX];
};

struct modepid {
    yobd_mode mode;
    yobd_pid pid;
    float min;
    float max;
    size_t count;
};

static struct modepid s_obd_rqs[] = {
    {
        .mode = 0x01,
        .pid = 0x000c,
        .min = 0,
        .max = 16383.75,
        .count = 0
    },
    {
        .mode = 0x01,
        .pid = 0x000d,
        .min = 0,
        .max = 255.0,
        .count = 0
    }
};

static struct test_ctx s_ctx = {
    .seqno = 0,
    .count = ARRAYLEN(s_obd_rqs),
    .obd_rqs = s_obd_rqs
};

void data_cb(const struct hound_record *record, hound_seqno seqno, void *data)
{
    struct test_ctx *ctx;
    const char *dev_name;
    hound_err err;
    size_t i;
    yobd_mode mode;
    struct modepid *modepid;
    yobd_pid pid;
    float val;

    XASSERT_NOT_NULL(record);
    XASSERT_NOT_NULL(record->data);
    XASSERT_EQ(record->size, sizeof(float));
    XASSERT_NOT_NULL(data);

    ctx = data;

    XASSERT_EQ(ctx->seqno, seqno);

    hound_obd_get_mode_pid(record->data_id, &mode, &pid);

    for (i = 0; i < ctx->count; ++i) {
        modepid = &ctx->obd_rqs[i];
        if (modepid->mode == mode && modepid->pid == pid) {
            ++modepid->count;
            val = *((float *) record->data);
            XASSERT_GTE(val, modepid->min);
            XASSERT_LTE(val, modepid->max);
        }
    }

    err = hound_get_dev_name(record->dev_id, &dev_name);
    XASSERT_OK(err);
    XASSERT_STREQ(dev_name, ctx->iface);

    ++ctx->seqno;
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
    struct modepid *modepid;
    size_t n;
    struct hound_data_rq *data_rq;
    struct hound_data_rq data_rqs[s_ctx.count];
    struct hound_rq rq = {
        .queue_len = 10000,
        .cb = data_cb,
        .cb_ctx = &s_ctx,
        .rq_list.len = ARRAYLEN(data_rqs),
        .rq_list.data = data_rqs
    };

    for (i = 0; i < ARRAYLEN(data_rqs); ++i) {
        data_rq = &data_rqs[i];
        modepid = &s_ctx.obd_rqs[i];
        modepid->count = 0;
        hound_obd_get_data_id(modepid->mode, modepid->pid, &data_rq->id);
        data_rq->period_ns = period_ns;
    }

    s_ctx.seqno = 0;
    err = hound_alloc_ctx(&rq, &ctx);
    XASSERT_OK(err);

    err = hound_start(ctx);
    XASSERT_OK(err);

    if (RUNNING_ON_VALGRIND) {
        n = 2;
    }
    else {
        n = 100;
    }
    for (i = 0; i < n; ++i) {
        if (period_ns == 0) {
            err = hound_next(ctx, 1);
            XASSERT_OK(err);
        }
        err = hound_read(ctx, s_ctx.count, NULL);
        XASSERT_OK(err);
    }

    for (i = 1; i < s_ctx.count; ++i) {
        XASSERT_EQ(s_ctx.obd_rqs[0].count, s_ctx.obd_rqs[i].count);
    }

    err = hound_stop(ctx);
    XASSERT_OK(err);

    err = hound_free_ctx(ctx);
    XASSERT_OK(err);
}

static pid_t child_pid = -1;

void sig_handler(int sig)
{
    /* Propagate the signal to the child process. */
    kill(child_pid, sig);
}

static
pid_t start_sim(const char *iface, const char *schema_file, const char *sim_path)
{
    int err;
    pid_t pid;
    sem_t *sem;
    static const char *sem_name;
    struct sigaction sigact;

    /*
     * Propagate kill signals to the children so we don't leave zombie processes
     * if we crash.
     */
    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = sig_handler;
    sigact.sa_flags = 0;
    sigaction(SIGABRT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    sem_name = "/hound-obd-sim";
    sem = sem_open(sem_name, O_CREAT, 0644, 0);
    XASSERT_NEQ((void *) sem, (void *) SEM_FAILED);

    child_pid = pid = fork();
    XASSERT_NEQ(pid, -1);
    if (pid == 0) {
        /* Child. */
        err = execl(sim_path, sim_path, iface, schema_file, sem_name, NULL);
        XASSERT_NEQ(err, -1);
    }

    /* Wait for the OBD simulator to be ready to receive data. */
    sem_wait(sem);

    /*
     * We use this semaphore until to know when the simulation has started, so
     * we can destroy it now.
     */
    sem_close(sem);
    sem_unlink(sem_name);

    return pid;
}

static
void stop_sim(pid_t pid)
{
    int err;
    int status;

    err = kill(pid, SIGTERM);
    XASSERT_EQ(err, 0);

    err = waitpid(pid, &status, 0);
    XASSERT_EQ(err, pid);
    XASSERT(WIFEXITED(status));
}

int main(int argc, const char **argv)
{
    hound_err err;
    struct hound_init_arg init;
    const char *schema_base;
    pid_t pid;
    const char *sim_path;
    const char *yobd_schema;

    if (argc != 4) {
        fprintf(
            stderr,
            "Usage: %s CAN-IFACE SIM-PATH SCHEMA-BASE-PATH\n",
            argv[0]);
        exit(EXIT_FAILURE);
    }

    if (strnlen(argv[1], IFNAMSIZ) == IFNAMSIZ) {
        fprintf(stderr, "Device argument is longer than IFNAMSIZ\n");
        exit(EXIT_FAILURE);
    }
    strcpy(s_ctx.iface, argv[1]); /* NOLINT, string size already checked */

    if (strnlen(argv[2], PATH_MAX) == PATH_MAX) {
        fprintf(stderr, "obd simulator path is longer than PATH_MAX\n");
        exit(EXIT_FAILURE);
    }
    sim_path = argv[2];

    if (strnlen(argv[3], PATH_MAX) == PATH_MAX) {
        fprintf(stderr, "Schema base path is longer than PATH_MAX\n");
        exit(EXIT_FAILURE);
    }
    schema_base = argv[3];

    if (!can_iface_exists(s_ctx.iface)) {
        fprintf(
            stderr,
            "Failed to open CAN interface %s\n"
            "Run this command to create a CAN interface:\n"
            "sudo meson/vcan setup\n",
            s_ctx.iface);
        exit(EXIT_FAILURE);
    }

    yobd_schema = "sae-standard.yaml";
    init.type = HOUND_TYPE_BYTES;
    init.data.as_bytes = (unsigned char *) yobd_schema;
    err = hound_init_driver(
        "obd",
        s_ctx.iface,
        schema_base,
        "sae-standard.yaml",
        1,
        &init);
    XASSERT_OK(err);

    pid = start_sim(s_ctx.iface, yobd_schema, sim_path);

    /* On-demand data. */
    test_read(0);

    /* Periodic data. */
    test_read(1e9/10000);

    stop_sim(pid);

    err = hound_destroy_driver(s_ctx.iface);
    XASSERT_OK(err);

    return EXIT_SUCCESS;
}
