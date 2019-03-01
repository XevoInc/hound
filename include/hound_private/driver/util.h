/**
 * @file      util.h
 * @brief     Optional driver utility code.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_DRIVER_UTIL_H_
#define HOUND_PRIVATE_DRIVER_UTIL_H_

#include <hound/hound.h>
#include <hound_private/driver.h>

hound_err drv_deepcopy_desc(
    struct hound_datadesc *dest,
    const struct hound_datadesc *src);

void drv_destroy_desc(struct hound_datadesc *desc);

#endif /* HOUND_PRIVATE_DRIVER_UTIL_H_ */
