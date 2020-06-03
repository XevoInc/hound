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
#include <unistd.h>
#include <xlib/xhash.h>
#include <xlib/xvec.h>

#define PAUSE_FD_INDEX 0
#define DATA_FD_START 1

#define READ_END 0
#define WRITE_END 1

#define POLL_BUF_SIZE (100*1024)
#define POLL_DEFAULT_EVENTS (POLLIN|POLLOUT|POLLPRI|POLLERR|POLLHUP)

struct pull_timeout_info {
    hound_data_id id;
    hound_data_period current_timeout;
    hound_data_period max_timeout;
};

struct pull_info {
    hound_data_period last_pull;
    xvec_t(struct pull_timeout_info) timeout_info;
};

/** Map from fd to pull-mode timeout information for the fd. */
XHASH_MAP_INIT_INT(PULL_MAP, struct pull_info)
xhash_t(PULL_MAP) *s_pull_map = NULL;

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
    bool timeout_enabled;
    hound_data_period timeout_ns;
    xvec_t(struct queue_entry) queues;
};

static struct {
    pthread_rwlock_t lock;
    xvec_t(struct fdctx) ctx;
    xvec_t(struct pollfd) fds;
} s_ios;

/* Polling. */
static pthread_t s_poll_thread;
static int s_self_pipe[2];
static pthread_mutex_t s_poll_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_poll_cond = PTHREAD_COND_INITIALIZER;
static volatile bool s_poll_active_target = false;
static volatile bool s_poll_active_current = false;

static unsigned char s_read_buf[POLL_BUF_SIZE];

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

void io_push_records(struct hound_record *records, size_t count)
{
    const struct hound_record *end;
    struct driver *drv;
    struct queue_entry *entry;
    struct fdctx *fdctx;
    size_t i;
    struct hound_record *record;
    struct record_info *rec_info;
    bool pushed;

    pthread_rwlock_rdlock(&s_ios.lock);

    drv = get_active_drv();
    XASSERT_NOT_NULL(drv);

    fdctx = get_fdctx(drv->fd);
    XASSERT_NOT_NULL(fdctx);

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
        record->dev_id = drv->id;
        memcpy(&rec_info->record, record, sizeof(*record));
        atomic_ref_init(&rec_info->refcount, 0);

        pushed = false;
        for (i = 0; i < xv_size(fdctx->queues); ++i) {
            entry = &xv_A(fdctx->queues, i);
            if (record->data_id == entry->id) {
                atomic_ref_inc(&rec_info->refcount);
                queue_push(entry->queue, rec_info);
                pushed = true;
            }
        }
        if (!pushed) {
            /*
             * This is unlikely, but if there's no queue associated with this
             * data, make sure we don't leak the record info. This should happen
             * only if a driver pushes data from outside the poll loop and its
             * context is being modified at the same time.
             */
            drv_free(rec_info);
        }
    }

    pthread_rwlock_unlock(&s_ios.lock);
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
hound_err make_records(struct driver *drv, unsigned char *buf, size_t size)
{
    ssize_t bytes_read;
    hound_err err;
    int fd;

    fd = drv_fd();
    bytes_read = read(fd, buf, size);
    if (bytes_read <= 0) {
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

    /*
     * NOTE: We don't use drv_ops_parse here, which would set the active
     * driver and take the driver ops mutex. This is because we are already
     * inside a driver ops callback, so re-taking the mutex will cause a
     * deadlock!
     */
    err = drv->ops.parse(buf, bytes_read);
    if (err != HOUND_OK) {
        hound_log_err(
                err,
                "Driver failed to parse records (size = %zu, drv = 0x%p)",
                bytes_read, drv);
        return err;
    }

    return HOUND_OK;
}

hound_err io_default_pull(
    short events,
    short *next_events,
    bool *timeout_enabled,
    hound_data_period *timeout)
{
    struct driver *drv;
    hound_err err;
    size_t i;
    struct pull_info *info;
    xhiter_t iter;
    hound_data_period lateness;
    hound_data_period min_timeout;
    hound_data_period now;
    struct pull_timeout_info *timeout_info;
    hound_data_period time_since_last_poll;

    now = get_time_ns();
    drv = get_active_drv();

    iter = xh_get(PULL_MAP, s_pull_map, drv_fd());
    XASSERT_NEQ(iter, xh_end(s_pull_map));
    info = &xh_val(s_pull_map, iter);

    /* Adjust our timeout data and trigger a pull if a timeout expired. */
    time_since_last_poll = now - info->last_pull;
    min_timeout = UINT64_MAX;
    for (i = 0; i < xv_size(info->timeout_info); ++i) {
        timeout_info = &xv_A(info->timeout_info, i);

        if (time_since_last_poll >= timeout_info->current_timeout) {
            /* Driver is ready to pull data. */
            lateness = time_since_last_poll - timeout_info->current_timeout;
            /*
             * NOTE: We don't use drv_ops_next here, which would set the active
             * driver and take the driver ops mutex. This is because we are already
             * inside a driver ops callback, so re-taking the mutex will cause a
             * deadlock!
             */
            err = drv->ops.next(timeout_info->id);
            if (err != HOUND_OK) {
                hound_log_err(
                        err,
                        "driver %p failed to pull data",
                        (void *) drv);
            }
            if (lateness >= timeout_info->max_timeout) {
                /* We were so late that the driver is ready again. */
                timeout_info->current_timeout = 0;
            }
            else {
                timeout_info->current_timeout =
                    timeout_info->max_timeout - lateness;
            }
        }
        else {
            timeout_info->current_timeout -= time_since_last_poll;
        }

        /* Find the next lowest timeout. */
        min_timeout = min(min_timeout, timeout_info->current_timeout);
    }

    if (events & POLLIN) {
        err = make_records(drv, s_read_buf, ARRAYLEN(s_read_buf));
    }
    else {
        err = HOUND_OK;
    }


    *next_events = POLLIN;
    *timeout_enabled = true;
    *timeout = min_timeout;
    info->last_pull = now;

    return err;
}

hound_err io_default_push(
    short events,
    short *next_events,
    bool *timeout_enabled,
    UNUSED hound_data_period *timeout)
{
    *next_events = POLLIN;
    *timeout_enabled = false;

    if (!(events & POLLIN)) {
        return HOUND_OK;
    }

    return make_records(get_active_drv(), s_read_buf, ARRAYLEN(s_read_buf));
}

static
hound_err io_read(struct fdctx *ctx, short events, short *next_events)
{
    hound_err err;

    ctx->timeout_enabled = false;
    ctx->timeout_ns = UINT64_MAX;
    err = drv_op_poll(
        ctx->drv,
        events,
        next_events,
        &ctx->timeout_enabled,
        &ctx->timeout_ns);
    if (err != HOUND_OK) {
        return err;
    }

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
    bool result;

    pthread_rwlock_rdlock(&s_ios.lock);

    pfd = &xv_A(s_ios.fds, PAUSE_FD_INDEX);
    if (!(pfd->revents & POLLIN)) {
        result = false;
        goto out;
    }

    /* Read the self-pipe so it can be used again. */
    do {
        bytes = read(pfd->fd, &buf, sizeof(buf));
        XASSERT_NEQ(bytes, -1);
    } while (bytes != sizeof(buf));

    result = true;

out:
    pthread_rwlock_unlock(&s_ios.lock);
    return result;
}

static
void *io_poll(UNUSED void *data)
{
    struct fdctx *ctx;
    hound_err err;
    bool fd_timeout;
    size_t i;
    int fds;
    bool have_timeout;
    uint_fast64_t last_poll_ns;
    hound_data_period min_timeout;
    hound_data_period now;
    struct pollfd *pfd;
    struct timespec *timeout;
    struct timespec timeout_spec;
    hound_data_period time_since_last_poll;

    last_poll_ns = get_time_ns();

    while (true) {
        io_wait_for_ready();

        /* Find the timeout we need for the poll (if any). */
        have_timeout = false;
        min_timeout = UINT64_MAX;
        for (i = 0; i < xv_size(s_ios.ctx); ++i) {
            ctx = &xv_A(s_ios.ctx, i);
            if (!ctx->timeout_enabled) {
                continue;
            }
            have_timeout = true;
            min_timeout = min(min_timeout, ctx->timeout_ns);
        }
        if (have_timeout) {
            populate_timespec(min_timeout, &timeout_spec);
            timeout = &timeout_spec;
        }
        else {
            timeout = NULL;
        }

        /*
         * Wait for I/O. We use ppoll for a more precise timeout, not because we
         * need to care about signals.
         */
        fds = ppoll(xv_data(s_ios.fds), xv_size(s_ios.fds), timeout, NULL);
        now = get_time_ns();
        time_since_last_poll = now - last_poll_ns;
        last_poll_ns = now;
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

        /* Read all fds that have data, and adjust timeouts. */
        pthread_rwlock_rdlock(&s_ios.lock);
        for (i = DATA_FD_START; i < xv_size(s_ios.fds); ++i) {
            ctx = get_fdctx_from_fd_index(i);
            pfd = &xv_A(s_ios.fds, i);

            if (ctx->timeout_enabled) {
                if (time_since_last_poll >= ctx->timeout_ns) {
                    ctx->timeout_enabled = false;
                    fd_timeout = true;
                }
                else {
                    ctx->timeout_ns -= time_since_last_poll;
                }
            }
            else {
                fd_timeout = false;
            }

            if (pfd->revents == 0 && !fd_timeout) {
                continue;
            }

            err = io_read(ctx, pfd->revents, &pfd->events);
            if (err == HOUND_INTR) {
                /* Someone wants to pause polling; finish reading later. */
                break;
            }
            if (err != HOUND_OK) {
                hound_log_err(err, "Failed to grab record from fd %d", pfd->fd);
                continue;
            }
        }
        pthread_rwlock_unlock(&s_ios.lock);
    }

    return NULL;
}

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
    struct pull_info *info;
    xhiter_t iter;
    struct pollfd *pfd;
    int ret;

    /*
     * Polling *must* have been paused before calling this function, as we
     * modify the poll structures here.
     */

    XASSERT_NOT_NULL(drv);
    XASSERT_NEQ(fd, 0);

    /* Our fds must be non-blocking for the poll loop to work. */
    flags = fcntl(fd, F_GETFL, 0);
    XASSERT_NEQ(flags, -1);
    err = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    XASSERT_NEQ(err, -1);

    pthread_rwlock_wrlock(&s_ios.lock);

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
    ctx->timeout_enabled = false;
    ctx->timeout_ns = UINT64_MAX;
    xv_init(ctx->queues);

    if (driver_is_pull_mode(drv)) {
        iter = xh_put(PULL_MAP, s_pull_map, fd, &ret);
        if (ret == -1) {
            err = HOUND_OOM;
            goto error_pull_mode_indices_push;
        }
        info = &xh_val(s_pull_map, iter);

        info->last_pull = 0;
        xv_init(info->timeout_info);
    }

    err = HOUND_OK;
    goto out;

error_pull_mode_indices_push:
    (void) xv_pop(s_ios.ctx);
error_ctx_push:
    (void) xv_pop(s_ios.fds);
error_fds_push:
out:
    pthread_rwlock_unlock(&s_ios.lock);
    return err;
}

void io_remove_fd(int fd)
{
    struct fdctx *ctx;
    size_t ctx_index;
    size_t fd_index;
    struct pull_info *info;
    xhiter_t iter;

    /*
     * Polling *must* have been paused before calling this function, as we
     * modify the poll structures here.
     */

    pthread_rwlock_wrlock(&s_ios.lock);

    fd_index = get_fd_index(fd);
    ctx_index = get_fdctx_index(fd_index);
    ctx = get_fdctx_from_fd_index(fd_index);
    XASSERT_NOT_NULL(ctx);

    if (driver_is_pull_mode(ctx->drv)) {
        /* If we have a pointer to this index in the pull mode map, remove it. */
        iter = xh_get(PULL_MAP, s_pull_map, fd);
        if (iter != xh_end(s_pull_map)) {
            info = &xh_val(s_pull_map, iter);
            xv_destroy(info->timeout_info);
            xh_del(PULL_MAP, s_pull_map, iter);
        }
    }

    /* Remove fd and ctx. */
    RM_VEC_INDEX(s_ios.fds, fd_index);
    RM_VEC_INDEX(s_ios.ctx, ctx_index);

    xv_destroy(ctx->queues);

    pthread_rwlock_unlock(&s_ios.lock);
}

static
void set_fd_timeout(int fd, struct fdctx *ctx)
{
    size_t i;
    struct pull_info *info;
    xhiter_t iter;
    hound_data_period min_timeout;
    struct pull_timeout_info *timeout_info;

    /*
     * This function should be called only for pull-mode drivers, so we should
     * always find our fd in the pull-mode map.
     */
    iter = xh_get(PULL_MAP, s_pull_map, fd);
    XASSERT_NEQ(iter, xh_end(s_pull_map));
    info = &xh_val(s_pull_map, iter);

    if (xv_size(info->timeout_info) == 0) {
        /* No timing entries; nothing to do here. */
        ctx->timeout_enabled = false;
        ctx->timeout_ns = UINT64_MAX;
        return;
    }

    min_timeout = UINT64_MAX;
    for (i = 0; i < xv_size(info->timeout_info); ++i) {
        timeout_info = &xv_A(info->timeout_info, i);
        min_timeout = min(min_timeout, timeout_info->current_timeout);
    }
    ctx->timeout_enabled = true;
    ctx->timeout_ns = min_timeout;
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
    struct pull_info *info;
    xhiter_t iter;
    size_t j;
    size_t queue_count;
    struct hound_data_rq *rq;
    struct pull_timeout_info *timeout_info;

    /*
     * Polling *must* have been paused before calling this function, as we
     * modify the poll structures here.
     */

    pthread_rwlock_wrlock(&s_ios.lock);

    ctx = get_fdctx(fd);
    XASSERT_NOT_NULL(ctx);

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

        if (driver_is_pull_mode(ctx->drv) && rq->period_ns > 0) {
            /*
             * Push-mode doesn't need timeout data, as the driver manages the
             * data timing.
             *
             * Further, a period of 0 has special meaning: data is "on-demand",
             * meaning neither push nor pull (no data flows until requested with
             * hound_next(). Therefore, if the period is 0, we should not add
             * timing data for this request.
             */
            iter = xh_get(PULL_MAP, s_pull_map, fd);
            XASSERT_NEQ(iter, xh_end(s_pull_map));
            info = &xh_val(s_pull_map, iter);
            timeout_info = xv_pushp(struct pull_timeout_info, info->timeout_info);
            if (timeout_info == NULL) {
                err = HOUND_OOM;
                for (j = 0; j < queue_count; ++j) {
                    (void) xv_pop(ctx->queues);
                }
                goto out;
            }

            timeout_info->id = entry->id;
            timeout_info->current_timeout = rq->period_ns;
            timeout_info->max_timeout = rq->period_ns;
        }
    }

    /*
     * For pull-mode drivers, adjust the new fd timeout, as some timing entries
     * have been deleted.
     */
    if (driver_is_pull_mode(ctx->drv)) {
        set_fd_timeout(fd, ctx);
    }

out:
    pthread_rwlock_unlock(&s_ios.lock);
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
    struct pull_info *info;
    xhiter_t iter;
    size_t j;
    struct hound_data_rq *rq;
    struct pull_timeout_info *timeout_info;

    /*
     * Polling *must* have been paused before calling this function, as we
     * modify the poll structures here.
     */

    pthread_rwlock_wrlock(&s_ios.lock);

    ctx = get_fdctx(fd);
    XASSERT_NOT_NULL(ctx);

    /* Remove all matching queue entries. */
    iter = xh_get(PULL_MAP, s_pull_map, fd);
    XASSERT_NEQ(iter, xh_end(s_pull_map));
    info = &xh_val(s_pull_map, iter);
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
        /* We should have removed at least one queue entry. */
        XASSERT_LT(j, xv_size(ctx->queues));

        if (driver_is_pull_mode(ctx->drv) && rq->period_ns > 0) {
            /* Remove pull-mode timing info, if any. */
            for (j = 0; j < xv_size(info->timeout_info); ++j) {
                timeout_info = &xv_A(info->timeout_info, j);
                if (timeout_info->id == rq->id &&
                    timeout_info->max_timeout == rq->period_ns) {
                    RM_VEC_INDEX(info->timeout_info, j);
                    break;
                }
            }
            /*
             * We should have found a timing entry, as this is a pull-mode data
             * ID.
             */
            XASSERT_LT(j, xv_size(info->timeout_info));
        }
    }

    /*
     * For pull-mode drivers, adjust the new fd timeout, as some timing entries
     * have been deleted.
     */
    if (driver_is_pull_mode(ctx->drv)) {
        set_fd_timeout(fd, ctx);
    }

    pthread_rwlock_unlock(&s_ios.lock);
}

void io_init(void)
{
    hound_err err;
    struct pollfd *pfd;
    int ret;

    pthread_rwlock_init(&s_ios.lock, NULL);
    xv_init(s_ios.ctx);
    xv_init(s_ios.fds);

    s_pull_map = xh_init(PULL_MAP);
    if (s_pull_map == NULL) {
        hound_log_nofmt(XLOG_ERR, "Failed to initialize pull-mode timing map");
        return;
    }

    /*
     * Create our self-pipe, which we use to interrupt the poll loop when
     * needed. Mark it non-blocking so it can't block a read() during the poll
     * loop. It really never should block, but this is a defensive precaution.
     */
    ret = pipe2(s_self_pipe, O_NONBLOCK);
    if (ret != 0) {
        hound_log_nofmt(XLOG_ERR, "Failed to create self pipe");
        return;
    }

    pfd = xv_pushp(struct pollfd, s_ios.fds);
    if (pfd == NULL) {
        hound_log_nofmt(XLOG_ERR, "Failed to setup pfd for self pipe");
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
    xv_destroy(s_ios.ctx);
    xv_destroy(s_ios.fds);
    pthread_rwlock_destroy(&s_ios.lock);
    xh_destroy(PULL_MAP, s_pull_map);
    close(s_self_pipe[READ_END]);
    close(s_self_pipe[WRITE_END]);
}
