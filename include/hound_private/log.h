/**
 * @file      log.h
 * @brief     Private logging header, for functions that should not be publicly
 *            exported.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_LOG_H_
#define HOUND_PRIVATE_LOG_H_

#include <stdarg.h>

void log_init(void);
void log_destroy(void);


void log_msg(int priority, const char *fmt, ...);
void log_vmsg(int priority, const char *fmt, va_list args);

#endif /* HOUND_PRIVATE_LOG_H_ */
