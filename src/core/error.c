/**
 * @file      error.c
 * @brief     Hound error handling functions.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/error.h>
#include <hound/hound.h>
#include <hound/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#define STATIC_STRLEN(s) (sizeof(char)*sizeof(s) - 1)
#define MAX_EXPR_STR_LEN (100)
#define MAX_ARG_STR_LEN (1000)

#define MAX_FORMAT_STR_LEN  ( \
    STATIC_STRLEN(_HOUND_ASSERT_STR_BASE) + \
    MAX_EXPR_STR_LEN + \
    STATIC_STRLEN(s_loc_str) + \
    MAX_ARG_STR_LEN \
    )

static const char s_loc_str[] = _HOUND_ASSERT_STR_LOC_DETAILS "\n";

void _error_log_msg(
    const char *expr,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    va_list args)
{
    char msg[MAX_FORMAT_STR_LEN];
    char *pos;

    /*
     * We check only the sprintfs for strings that are unknown at compile-time
     * because they have the potential to be variable length.
     */
    pos = msg;
    pos += sprintf(pos, _HOUND_ASSERT_STR_BASE);
    /*
     * Override the -Wformat-security warning here regarding snprintf on
     * untrusted input because expr comes from a limited set of compile-time
     * assert macros, which are all safe.
     */
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wformat-security"
    pos += snprintf(pos, MAX_EXPR_STR_LEN, expr);
    #pragma GCC diagnostic pop
    pos += sprintf(pos, s_loc_str, file, line, func);
    vsnprintf(pos, MAX_ARG_STR_LEN, fmt, args);

    hound_log_msg(LOG_CRIT, msg);
}

const char *error_strerror(hound_err err)
{
    /* Not a hound error code. */
    if (err > 0) {
        return strerror(err);
    }

    switch ((enum hound_err_enum) err) {
        case HOUND_OK:
            return "OK";
        case HOUND_NULL_VAL:
            return "NULL value specified";
        case HOUND_OOM:
            return "out of memory!";
        case HOUND_DRIVER_ALREADY_REGISTERED:
            return "driver is already registered";
        case HOUND_DRIVER_NOT_REGISTERED:
            return "driver is not registered";
        case HOUND_DRIVER_IN_USE:
            return "driver is in-use";
        case HOUND_MISSING_DEVICE_IDS:
            return "driver specifies NULL device IDs";
        case HOUND_CONFLICTING_DRIVERS:
            return "two drivers registered for the same data ID";
        case HOUND_NO_DATA_REQUESTED:
            return "context does not request any data";
        case HOUND_DATA_ID_DOES_NOT_EXIST:
            return "context requests data ID not registered with a driver";
        case HOUND_CTX_ALREADY_ACTIVE:
            return "context has already been started";
        case HOUND_CTX_NOT_ACTIVE:
            return "context has not been started";
        case HOUND_EMPTY_QUEUE:
            return "context requests a data queue of length 0";
        case HOUND_MISSING_CALLBACK:
            return "context does not specify a data callback";
        case HOUND_FREQUENCY_UNSUPPORTED:
            return "context requests a frequency not supported by the backing driver";
        case HOUND_IO_ERROR:
            return "I/O error (EIO)";
        case HOUND_QUEUE_TOO_SMALL:
            return "blocking read requested for more samples than the max queue size";
    }

    __builtin_unreachable();
}
