/**
 * @file      log.c
 * @brief     Hound logging subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _DEFAULT_SOURCE
#include <hound_private/log.h>
#include <syslog.h>

void log_msg(int priority, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    log_vmsg(priority, fmt, args);
    va_end(args);
}

void log_vmsg(int priority, const char *fmt, va_list args)
{
    vsyslog(priority, fmt, args);
}

void log_assert_msg(const char *msg)
{
    log_msg(LOG_CRIT, msg);
}

void log_init(void)
{
    /* TODO: Make log levels configurable. */
    setlogmask(LOG_UPTO(LOG_INFO));
    /*
     * Immediately initialize the logger to avoid I/O latency when we first
     * initialize the logger.
     */
    openlog("hound", LOG_NDELAY|LOG_PERROR|LOG_CONS|LOG_PID, LOG_SYSLOG);
}

void log_destroy(void)
{
    closelog();
}
