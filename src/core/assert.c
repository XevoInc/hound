/**
 * @file      assert.c
 * @brief     Hound assertions.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/log.h>
#include <hound/assert.h>
#include <stdarg.h>
#include <stdio.h>
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

void _assert_log_msg(
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
