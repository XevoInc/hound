/**
 * @file      assert.h
 * @brief     Private hound assertions.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_ASSERT_H_
#define HOUND_PRIVATE_ASSERT_H_

#include <stdarg.h>

void _assert_log_msg(
    const char *expr,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    va_list args);

#endif /* HOUND_PRIVATE_ASSERT_H_ */
