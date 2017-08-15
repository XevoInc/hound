/**
 * @file      log.h
 * @brief     Logging header for use by drivers.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_LOG_H_
#define HOUND_LOG_H_

#include <hound/error.h>
#include <stdarg.h>
#include <syslog.h>

void hound_log_msg(int priority, const char *fmt, ...);
void hound_log_vmsg(int priority, const char *fmt, va_list args);

/*
 * Sadly, we have two versions of log_err because having empty __VA_ARGS__ in a
 * vararg macro is banned by C99.
 */
#define hound_log_err(err, fmt, ...) \
    do { \
        hound_log_msg(LOG_ERR, fmt ", err: %d (%s)'", __VA_ARGS__, err, hound_strerror(err)); \
    } while (0);

#define hound_log_err_nofmt(err, msg) \
    do { \
        hound_log_msg(LOG_ERR, msg ", err: %d (%s)", err, hound_strerror(err)); \
    } while (0);

#endif /* HOUND_LOG_H_ */
