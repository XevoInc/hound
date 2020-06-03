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
