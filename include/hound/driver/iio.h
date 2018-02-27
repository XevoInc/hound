/**
 * @file      iio.h
 * @brief     Public Industrial I/O driver header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2018 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_DRIVER_IIO_H_
#define HOUND_DRIVER_IIO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <hound/hound.h>

struct hound_iio_driver_init {
    const char *dev;
    uint_fast64_t buf_ns;
};

hound_err hound_register_iio_driver(const struct hound_iio_driver_init *init);

#ifdef __cplusplus
}
#endif

#endif /* HOUND_DRIVER_IIO_H_ */
