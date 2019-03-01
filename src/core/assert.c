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

static const char s_loc_str[] = _HOUND_ASSERT_STR_LOC_DETAILS "\n";
static const size_t s_max_str_len =
    STATIC_STRLEN(_HOUND_ASSERT_STR_BASE) +
    MAX_EXPR_STR_LEN +
    STATIC_STRLEN(s_loc_str) +
    MAX_ARG_STR_LEN;

void _assert_log_msg(
    const char *expr,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    va_list args)
{
    char msg[s_max_str_len];
    char *pos;

    /*
     * We check only the sprintfs for strings that are unknown at compile-time
     * because they have the potential to be variable length.
     */
    pos = msg;
    pos += sprintf(pos, _HOUND_ASSERT_STR_BASE);
    pos += snprintf(pos, MAX_EXPR_STR_LEN, expr);
    pos += sprintf(pos, s_loc_str, file, line, func);
    vsnprintf(pos, MAX_ARG_STR_LEN, fmt, args);

    hound_log_msg(LOG_CRIT, msg);
}
