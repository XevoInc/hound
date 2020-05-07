/**
 * @file      io.c
 * @brief     Hound I/O subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <hound-private/driver.h>
#include <hound-private/driver-ops.h>
#include <hound-private/error.h>
#include <hound-private/log.h>
#include <hound-private/queue.h>
#include <hound-private/refcount.h>
#include <hound-private/util.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <xlib/xvec.h>

#define PAUSE_FD_INDEX 0
#define DATA_FD_START 1

#define READ_END 0
#define WRITE_END 1

#define POLL_BUF_SIZE (100*1024)
#define POLL_DEFAULT_EVENTS (POLLIN|POLLOUT|POLLPRI|POLLERR|POLLHUP)

struct pull_timing_entry {
    hound_data_id id;
    hound_data_period current_timeout;
    hound_data_period max_timeout;
};

struct queue_entry {
    hound_data_id id;
    struct queue *queue;
};

/**
 * Provides the relevant information that the I/O system need to know about a
 * given fd (besides the fd value itself). This is stored separately from the
 * struct pollfd structures because poll requires that the array of struct
 * pollfd's passed in be contiguous in memory.
 */
struct fdctx {
    struct driver *drv;
    xvec_t(struct pull_timing_entry) timings;
    xvec_t(struct queue_entry) queues;
};

static struct {
    xvec_t(struct fdctx) ctx;
    xvec_t(struct pollfd) fds;
    xvec_t(size_t) pull_mode_indices;
    uint_fast64_t last_poll_ns;
} s_ios;

static pthread_t s_poll_thread;
static int s_self_pipe[2];
static pthread_mutex_t s_poll_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_poll_cond = PTHREAD_COND_INITIALIZER;
static volatile bool s_poll_active_target = false;
static volatile bool s_poll_active_current = false;

static unsigned char s_read_buf[POLL_BUF_SIZE];
static struct hound_record s_records[HOUND_DRIVER_MAX_RECORDS];

static
size_t get_fd_index(int fd)
{
    size_t index;
    const struct pollfd *pfd;

    for (index = DATA_FD_START; index < xv_size(s_ios.fds); ++index) {
        pfd = &xv_A(s_ios.fds, index);
        if (pfd->fd == fd) {
            break;
        }
    }
    XASSERT_NEQ(index, xv_size(s_ios.fds));

    return index;
}

static
size_t get_fdctx_index(size_t fd_index)
{
    /*
     * We have to subtract one to get the ctx corresponding to an fd index
     * because the first index is taken up by the special self-pipe that we use
     * to stop the poll loop.
     */
    XASSERT_GT(fd_index, 0);
    return fd_index - 1;
}

static
struct fdctx *get_fdctx(int fd)
{
    size_t fdctx_index;

    fdctx_index = get_fdctx_index(get_fd_index(fd));

    return &xv_A(s_ios.ctx, fdctx_index);
}

static
struct fdctx *get_fdctx_from_fd_index(size_t fd_index)
{
    size_t fdctx_index;

    fdctx_index = get_fdctx_index(fd_index);

    return &xv_A(s_ios.ctx, fdctx_index);
}

static
void push_records(struct fdctx *ctx, struct hound_record *records, size_t count)
{
    const struct hound_record *end;
    struct queue_entry *entry;
    size_t i;
    struct hound_record *record;
    struct record_info *rec_info;

    /* Add to all user queues. */
    end = records + count;
    for (record = records; record < end; ++record) {
        rec_info = drv_alloc(sizeof(*rec_info));
        if (rec_info == NULL) {
            hound_log_err_nofmt(
                HOUND_OOM,
                "Failed to allocate a rec_info; can't add record to user queue");
            continue;
        }
        record->dev_id = ctx->drv->id;
        memcpy(&rec_info->record, record, sizeof(*record));
        atomic_ref_init(&rec_info->refcount, 0);

        for (i = 0; i < xv_size(ctx->queues); ++i) {
            entry = &xv_A(ctx->queues, i);
            if (record->data_id == entry->id) {
                atomic_ref_inc(&rec_info->refcount);
                queue_push(entry->queue, rec_info);
            }
        }
    }
}

static
hound_err make_records(
    struct driver *drv,
    unsigned char *buf,
    size_t size,
    struct hound_record *records,
    size_t *record_count)
{
    size_t bytes_left;
    hound_err err;
    unsigned char *pos;

    /* Ask the driver to make records from the buffer. */
    pos = buf;
    bytes_left = size;
    while (bytes_left > 0) {
        *record_count = 0;
        /*
         * NOTE: We don't use drv_ops_parse here, which would set the active
         * driver and take the driver ops mutex. This is because we are already
         * inside a driver ops callback, so re-taking the mutex will cause a
         * deadlock!
         */
        err = drv->ops.parse(
            pos,
            &bytes_left,
            records,
            record_count);
        if (err != HOUND_OK) {
            hound_log_err(
                err,
                "Driver failed to parse records (size = %zu, drv = 0x%p)",
                bytes_left, drv);
            return err;
        }
        XASSERT_LTE(bytes_left, size);

        if (bytes_left == size) {
            /* Driver can't make more records from this buffer. We're done. */
            break;
        }

        XASSERT_GT(*record_count, 0);
        pos += size - bytes_left;
    }

    return HOUND_OK;
}

hound_err io_default_poll(
    short events,
    short *next_events,
    struct hound_record *records,
    size_t *record_count)
{
    ssize_t bytes_read;
    struct driver *drv;
    int fd;

    drv = get_active_drv();
    fd = drv_fd();

    if (!(events | POLLIN)) {
        *record_count = 0;
        return HOUND_OK;
    }

    bytes_read = read(fd, s_read_buf, ARRAYLEN(s_read_buf));
    if (bytes_read < 0) {
        if (bytes_read == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            /* No more data to read, so we're done. */
            return HOUND_OK;
        }

        /* Someone wanted to pause polling; we can finish reading later. */
        if (errno == EINTR) {
            return HOUND_INTR;
        }
        else if (errno == EIO) {
            hound_log_err(errno, "read returned EIO on fd %d", fd);
            return HOUND_IO_ERROR;
        }
        else {
            /* Other error codes are likely program bugs. */
            XASSERT_ERROR;
        }
    }

    *next_events = POLLIN;

    return make_records(
        drv,
        s_read_buf,
        bytes_read,
        records,
        record_count);
}

static
hound_err io_read(struct fdctx *ctx, short events, short *next_events)
{
    hound_err err;
    size_t record_count;

    record_count = 0;
    err = drv_op_poll(ctx->drv, events, next_events, s_records, &record_count);
    if (err != HOUND_OK) {
        return err;
    }

    push_records(ctx, s_records, record_count);

    return HOUND_OK;
}

/**
 * Wait until it's safe for the event loop to continue.
 */
static
void io_wait_for_ready(void) {
    pthread_mutex_lock(&s_poll_mutex);
    while (!s_poll_active_target || xv_size(s_ios.fds) == 0) {
        s_poll_active_current = false;
        pthread_cond_signal(&s_poll_cond);
        pthread_cond_wait(&s_poll_cond, &s_poll_mutex);
    }
    s_poll_active_current = true;
    pthread_mutex_unlock(&s_poll_mutex);
}

static
hound_data_period get_time_ns(void)
{
    struct timespec ts;

    /*
     * Use CLOCK_MONOTONIC_RAW, as it's not subject to time discontinuities due
     * to NTP, leap seconds, etc.
     */
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    return NSEC_PER_SEC*ts.tv_sec + ts.tv_nsec;
}

static
void populate_timespec(hound_data_period ts, struct timespec *spec)
{
    spec->tv_sec = ts / NSEC_PER_SEC;
    spec->tv_nsec = ts % NSEC_PER_SEC;
}

static
bool need_to_pause(void)
{
    char buf;
    ssize_t bytes;
    struct pollfd *pfd;

    pfd = &xv_A(s_ios.fds, PAUSE_FD_INDEX);
    if (!(pfd->revents & POLLIN)) {
        return false;
    }

    /* Read the self-pipe so it can be used again. */
    do {
        bytes = read(pfd->fd, &buf, sizeof(buf));
        XASSERT_NEQ(bytes, -1);
    } while (bytes != sizeof(buf));

    return true;
}

static
void *io_poll(UNUSED void *data)
{
    struct fdctx *ctx;
    hound_err err;
    size_t i;
    size_t j;
    int fds;
    hound_data_period lateness;
    hound_data_period min_timeout;
    hound_data_period now;
    struct pollfd *pfd;
    struct timespec *timeout;
    struct timespec timeout_spec;
    hound_data_period time_since_last_poll;
    struct pull_timing_entry *timing_entry;

    s_ios.last_poll_ns = get_time_ns();

    while (true) {
        io_wait_for_ready();

        /*
         * Find the timeout we need for the poll, in order to tell a pull-mode
         * driver to request data.
         */
        if (xv_size(s_ios.pull_mode_indices) > 0) {
            min_timeout = UINT64_MAX;
            for (i = 0; i < xv_size(s_ios.pull_mode_indices); ++i) {
                ctx = &xv_A(s_ios.ctx, xv_A(s_ios.pull_mode_indices, i));
                for (j = 0; j < xv_size(ctx->timings); ++j) {
                    timing_entry = &xv_A(ctx->timings, j);
                    min_timeout = min(
                        min_timeout,
                        timing_entry->current_timeout);
                }
            }
            populate_timespec(min_timeout, &timeout_spec);
            timeout = &timeout_spec;
            s_ios.last_poll_ns = get_time_ns();
        }
        else {
            timeout = NULL;
        }

        /*
         * Wait for I/O. We use ppoll for a more precise timeout, not because we
         * need to care about signals.
         */
        fds = ppoll(xv_data(s_ios.fds), xv_size(s_ios.fds), timeout, NULL);
        if (fds > 0 && need_to_pause()) {
            continue;
        }
        else if (fds == -1) {
            /* Error. */
            if (errno == EINTR) {
                /* We got a signal. Restart the syscall. */
                continue;
            }
            else if (errno == ENOMEM) {
                hound_log_err_nofmt(errno, "poll failed with ENOMEM");
            }
            else if (errno == EIO) {
                hound_log_err_nofmt(errno, "poll failed with EIO");
            }
            else {
                /* Other error codes are likely program bugs. */
                XASSERT_ERROR;
            }
        }

        /* Adjust our timeout data and tell drivers to pull data if needed. */
        if (xv_size(s_ios.pull_mode_indices) > 0) {
            now = get_time_ns();
            time_since_last_poll = now - s_ios.last_poll_ns;
            for (i = 0; i < xv_size(s_ios.pull_mode_indices); ++i) {
                ctx = &xv_A(s_ios.ctx, xv_A(s_ios.pull_mode_indices, i));
                for (j = 0; j < xv_size(ctx->timings); ++j) {
                    timing_entry = &xv_A(ctx->timings, j);
                    if (time_since_last_poll >= timing_entry->current_timeout) {
                        /* Driver is ready to pull data. */
                        lateness =
                            time_since_last_poll - timing_entry->current_timeout;
                        err = drv_op_next(ctx->drv, timing_entry->id);
                        if (err != HOUND_OK) {
                            hound_log_err(
                                err,
                                "driver %p failed to pull data",
                                (void *) ctx->drv);
                        }
                        if (lateness >= timing_entry->max_timeout) {
                            /* We were so late that the driver is ready again. */
                            timing_entry->current_timeout = 0;
                        }
                        else {
                            timing_entry->current_timeout =
                                timing_entry->max_timeout - lateness;
                        }
                    }
                    else {
                        timing_entry->current_timeout -= time_since_last_poll;
                    }
                }
            }
        }

        /* Read all fds that have data. */
        for (i = DATA_FD_START; fds > 0 && i < xv_size(s_ios.fds); ++i) {
            pfd = &xv_A(s_ios.fds, i);
            if (pfd->revents == 0) {
                continue;
            }

            ctx = get_fdctx_from_fd_index(i);
            err = io_read(ctx, pfd->revents, &pfd->events);
            if (err == HOUND_INTR) {
                /* Someone wants to pause polling; finish reading later. */
                break;
            }
            --fds;
            if (err != HOUND_OK) {
                hound_log_err(err, "Failed to grab record from fd %d", pfd->fd);
                continue;
            }
        }
    }

    return NULL;
}

static
void io_pause_poll(void)
{
    ssize_t bytes;
    static const char payload = 1;

    /*
     * Wait until the poll has actually canceled. io_wait_for_ready will signal
     * on the condition variable when it is run.
     */
    pthread_mutex_lock(&s_poll_mutex);
    while (s_poll_active_current) {
        s_poll_active_target = false;
        do {
            bytes = write(s_self_pipe[WRITE_END], &payload, sizeof(payload));
            XASSERT_NEQ(bytes, -1);
        } while (bytes != sizeof(payload));
        pthread_cond_signal(&s_poll_cond);
        pthread_cond_wait(&s_poll_cond, &s_poll_mutex);
    }
    pthread_mutex_unlock(&s_poll_mutex);
}

static
void io_resume_poll(void)
{
    pthread_mutex_lock(&s_poll_mutex);
    s_poll_active_target = true;
    pthread_cond_signal(&s_poll_cond);
    pthread_mutex_unlock(&s_poll_mutex);
}

static
hound_err io_start_poll(void)
{
    hound_err err;

    err = pthread_create(&s_poll_thread, NULL, io_poll, NULL);
    if (err != 0) {
        return err;
    }

    return HOUND_OK;
}

static
void io_stop_poll(void)
{
    hound_err err;
    void *ret;

    /* First let the event loop gracefully exit. */
    io_pause_poll();

    /* Now shoot it in the head. */
    err = pthread_cancel(s_poll_thread);
    XASSERT_EQ(err, 0);

    /* Wait until the thread is finally dead. */
    err = pthread_join(s_poll_thread, &ret);
    XASSERT_EQ(err, 0);
    XASSERT_EQ(ret, PTHREAD_CANCELED);
}

hound_err io_add_fd(int fd, struct driver *drv)
{
    struct fdctx *ctx;
    hound_err err;
    int flags;
    size_t *index;
    struct pollfd *pfd;

    XASSERT_NOT_NULL(drv);
    XASSERT_NEQ(fd, 0);

    /* Our fds must be non-blocking for the poll loop to work. */
    flags = fcntl(fd, F_GETFL, 0);
    XASSERT_NEQ(flags, -1);
    err = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    XASSERT_NEQ(err, -1);

    io_pause_poll();

    pfd = xv_pushp(struct pollfd, s_ios.fds);
    if (pfd == NULL) {
        err = HOUND_OOM;
        goto error_fds_push;
    }
    pfd->fd = fd;
    pfd->events = POLL_DEFAULT_EVENTS;

    ctx = xv_pushp(struct fdctx, s_ios.ctx);
    if (ctx == NULL) {
        err = HOUND_OOM;
        goto error_ctx_push;
    }
    ctx->drv = drv;
    xv_init(ctx->timings);
    xv_init(ctx->queues);

    if (drv->sched_mode == DRV_SCHED_PULL) {
        index = xv_pushp(size_t, s_ios.pull_mode_indices);
        if (index == NULL) {
            err = HOUND_OOM;
            goto error_pull_mode_indices_push;
        }
        *index = xv_size(s_ios.ctx) - 1;
    }

    err = HOUND_OK;
    goto out;

error_pull_mode_indices_push:
    (void) xv_pop(s_ios.ctx);
error_ctx_push:
    (void) xv_pop(s_ios.fds);
error_fds_push:
out:
    io_resume_poll();
    return err;
}

void io_remove_fd(int fd)
{
    struct fdctx *ctx;
    size_t ctx_index;
    hound_err err;
    int fds;
    size_t fd_index;
    size_t i;
    short next_events;
    struct pollfd pfd;

    fd_index = get_fd_index(fd);
    ctx_index = get_fdctx_index(fd_index);
    ctx = get_fdctx_from_fd_index(fd_index);
    XASSERT_NOT_NULL(ctx);

    io_pause_poll();

    if (ctx->drv->sched_mode == DRV_SCHED_PULL) {
        /*
         * Drain the fd of any buffered data. If we get interrupted, keep trying, as
         * we don't want to lose data. Note we don't do this for push-mode
         * drivers, as that could make this an infinite loop as the drivers
         * continually push more data into the fd. This is safe for pull-mode
         * drivers because we've already paused polling, so no more data should
         * be going into the fd.
         */
        pfd.fd = fd;
        pfd.events = POLLIN;
        while (true) {
            /*
             * Since we are returning immediately and don't need to care about
             * signals, we can safely use poll here instead of ppoll.
             */
            fds = poll(&pfd, 1, 0);
            if (fds == -1) {
                if (errno == EINTR) {
                    /* Interrupted; try again. */
                    continue;
                }
                else {
                    hound_log_err(errno, "failed to drain fd %d", fd);
                    break;
                }
            }

            break;
        }

        if (fds == 1) {
            err = io_read(ctx, pfd.revents, &next_events);
            if (err != HOUND_OK) {
                hound_log_err(err, "failed to read from fd %d", fd);
            }
        }

        /*
         * If we have a pointer to this index in the pull mode indices list, remove
         * it.
         */
        for (i = 0; i < xv_size(s_ios.pull_mode_indices); ++i) {
            if (xv_A(s_ios.pull_mode_indices, i) == ctx_index) {
                RM_VEC_INDEX(s_ios.pull_mode_indices, i);
                break;
            }
        }
    }

    /* Remove fd and ctx. */
    RM_VEC_INDEX(s_ios.fds, fd_index);
    RM_VEC_INDEX(s_ios.ctx, ctx_index);

    io_resume_poll();

    xv_destroy(ctx->timings);
    xv_destroy(ctx->queues);
}

hound_err io_add_queue(
    int fd,
    const struct hound_data_rq_list *rq_list,
    struct queue *queue)
{
    struct fdctx *ctx;
    struct queue_entry *entry;
    hound_err err;
    size_t i;
    size_t j;
    size_t queue_count;
    struct hound_data_rq *rq;
    struct pull_timing_entry *timing_entry;

    ctx = get_fdctx(fd);
    XASSERT_NOT_NULL(ctx);

    io_pause_poll();

    err = HOUND_OK;
    queue_count = 0;
    for (i = 0; i < rq_list->len; ++i) {
        /*
         * Add exactly one queue entry per data ID in this request. If we add
         * more than one queue entry, then the same record will get delivered
         * twice to the same queue, which should never happen. Note two
         * *different* user contexts can still have queue entries for the same
         * data ID, but the same data ID in *a single user context* should
         * result in just one queue entry.
         */
        rq = &rq_list->data[i];
        for (j = 0; j < i; ++j) {
            if (rq->id == rq_list->data[j].id) {
                /* We already added a queue entry, so don't add another. */
                break;
            }
        }
        if (j == i) {
            /* We haven't yet added a queue entry for this data ID. */
            entry = xv_pushp(struct queue_entry, ctx->queues);
            if (entry == NULL) {
                err = HOUND_OOM;
                goto out;
            }
            entry->id = rq->id;
            entry->queue = queue;
            ++queue_count;
        }

        if (ctx->drv->sched_mode == DRV_SCHED_PULL && rq->period_ns > 0) {
            /*
             * Push-mode doesn't need timeout data, as the driver manages the
             * data timing.
             *
             * Further, A period of 0 has special meaning: data is "on-demand",
             * meaning neither push nor pull (no data flows until requested with
             * hound_next(). Therefore, if the period is 0, we should not add
             * timing data for this request.
             */
            timing_entry = xv_pushp(struct pull_timing_entry, ctx->timings);
            if (timing_entry == NULL) {
                err = HOUND_OOM;
                for (j = 0; j < queue_count; ++j) {
                    (void) xv_pop(ctx->queues);
                    goto out;
                }
            }
            timing_entry->id = entry->id;
            timing_entry->current_timeout = rq->period_ns;
            timing_entry->max_timeout = rq->period_ns;
        }
    }

out:
    io_resume_poll();
    return err;
}

void io_remove_queue(
    int fd,
    const struct hound_data_rq_list *rq_list,
    struct queue *queue)
{
    struct fdctx *ctx;
    struct queue_entry *entry;
    size_t i;
    size_t j;
    struct hound_data_rq *rq;

    ctx = get_fdctx(fd);
    XASSERT_NOT_NULL(ctx);

    io_pause_poll();

    /* Remove all matching queue entries. */
    for (i = 0; i < rq_list->len; ++i) {
        rq = &rq_list->data[i];
        for (j = 0; j < xv_size(ctx->queues); ++j) {
            entry = &xv_A(ctx->queues, j);
            if (rq->id != entry->id || queue != entry->queue) {
                continue;
            }
            /* Remove the queue. */
            RM_VEC_INDEX(ctx->queues, j);
        }
        XASSERT_LT(j, xv_size(ctx->queues));
        break;
    }

    io_resume_poll();
}

void io_init(void)
{
    hound_err err;
    struct pollfd *pfd;
    int ret;

    xv_init(s_ios.ctx);
    xv_init(s_ios.fds);
    xv_init(s_ios.pull_mode_indices);
    s_ios.last_poll_ns = 0;

    /*
     * Create our self-pipe, which we use to interrupt the poll loop when
     * needed. Mark it non-blocking so it can't block a read() during the poll
     * loop. It really never should block, but this is a defensive precaution.
     */
    ret = pipe2(s_self_pipe, O_NONBLOCK);
    if (ret != 0) {
        log_msg(LOG_ERR, "Failed to create self pipe");
        return;
    }

    pfd = xv_pushp(struct pollfd, s_ios.fds);
    if (pfd == NULL) {
        log_msg(LOG_ERR, "Failed to setup pfd for self pipe");
        return;
    }
    pfd->fd = s_self_pipe[READ_END];
    pfd->events = POLLIN;

    err = io_start_poll();
    if (err != HOUND_OK) {
        hound_log_err_nofmt(err, "Failed io_start_poll");
        return;
    }
}

void io_destroy(void)
{
    io_stop_poll();
    xv_destroy(s_ios.pull_mode_indices);
    xv_destroy(s_ios.ctx);
    xv_destroy(s_ios.fds);
    close(s_self_pipe[READ_END]);
    close(s_self_pipe[WRITE_END]);
}
