/**
 * @file      error.h
 * @brief     Private hound error code.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_ERROR_H_
#define HOUND_PRIVATE_ERROR_H_

#include <hound/hound.h>
#include <hound_private/log.h>
#include <xlib/xassert.h>

const char *error_strerror(hound_err err);

#define XASSERT_ERRCODE(x, y) _XASSERT_ERRCODE(x, y, hound_strerror)
#define XASSERT_OK(err) XASSERT_ERRCODE(err, HOUND_OK)

#endif /* HOUND_PRIVATE_ERROR_H_ */
