/**
 * @file      log.h
 * @brief     Private logging header, for functions that should not be publicly
 *            exported.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_LOG_H_
#define HOUND_PRIVATE_LOG_H_

#include <xlib/xlog.h>

#define hound_log(pri, fmt, ...) xlog(pri, fmt, __VA_ARGS__)
#define hound_log_nofmt(pri, msg) xlog_nofmt(pri, msg)
#define hound_vlog(pri, fmt, args) xlog(pri, fmt, args)

/*
 * Sadly, we have two versions of log_err because having empty __VA_ARGS__ in a
 * vararg macro is banned by C99.
 */
#define hound_log_err(err, fmt, ...) \
    hound_log(XLOG_ERR, fmt ", err: %d (%s)'", __VA_ARGS__, err, hound_strerror(err))

#define hound_log_err_nofmt(err, msg) \
    hound_log(XLOG_ERR, msg ", err: %d (%s)", err, hound_strerror(err))

#endif /* HOUND_PRIVATE_LOG_H_ */
