/**
 * @file      util.c
 * @brief     Utility code.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound-private/log.h>
#include <hound-private/util.h>
#include <stdio.h>
#include <string.h>

size_t min(size_t a, size_t b)
{
    if (a < b) {
        return a;
    }
    else {
        return b;
    }
}

size_t max(size_t a, size_t b)
{
    if (a > b) {
        return a;
    }
    else {
        return b;
    }
}

hound_err norm_path(const char *base, const char *path, size_t len, char *out)
{
    int count;

    /* Return absolute paths as-is, and make relative paths relative to base. */
    if (path[0] == '/') {
        count = snprintf(out, len, "%s", path);
    }
    else {
        count = snprintf(out, len, "%s/%s", base, path);
    }

    if (count == (int) len) {
        hound_log_nofmt(
            XLOG_ERR,
            "Path is too long when joined with configuration directory");
        return HOUND_PATH_TOO_LONG;
    }

    return HOUND_OK;
}

void destroy_rq_list(struct hound_data_rq_list *rq_list)
{
    XASSERT_NOT_NULL(rq_list);
    free(rq_list->data);
}

void init_mutex(pthread_mutex_t *mutex)
{
    int rc;

    /* This is documented never to fail. */
    rc = pthread_mutex_init(mutex, NULL);
    XASSERT_EQ(rc, 0);
}

void destroy_mutex(pthread_mutex_t *mutex)
{
    int rc;

    /* This should never fail unless the mutex is currently held. */
    rc = pthread_mutex_destroy(mutex);
    XASSERT_EQ(rc, 0);
}

void init_cond(pthread_cond_t *cond)
{
    int rc;

    /* This is documented never to fail. */
    rc = pthread_cond_init(cond, NULL);
    XASSERT_EQ(rc, 0);
}

void destroy_cond(pthread_cond_t *cond)
{
    int rc;

    /* This is never fail unless someone is waiting on this condition. */
    rc = pthread_cond_destroy(cond);
    XASSERT_EQ(rc, 0);
}

void lock_mutex(pthread_mutex_t *mutex)
{
    int rc;

    /* If the mutex is valid, this should never fail. */
    rc = pthread_mutex_lock(mutex);
    XASSERT_EQ(rc, 0);
}

void unlock_mutex(pthread_mutex_t *mutex)
{
    int rc;

    /* If the mutex is valid, this should never fail. */
    rc = pthread_mutex_unlock(mutex);
    XASSERT_EQ(rc, 0);
}

void cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    int rc;

    /* This routine must be called with the associated mutex held. */

    /* If the condition variable is valid, this should never fail. */
    rc = pthread_cond_wait(cond, mutex);
    XASSERT_EQ(rc, 0);
}

void cond_signal(pthread_cond_t *cond)
{
    int rc;

    /* If the condition variable is valid, this should never fail. */
    rc = pthread_cond_signal(cond);
    XASSERT_EQ(rc, 0);
}
