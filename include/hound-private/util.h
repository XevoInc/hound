/**
 * @file      util.h
 * @brief     Utility functions.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_UTIL_H_
#define HOUND_PRIVATE_UTIL_H_

#include <hound/hound.h>
#include <stddef.h>
#include <xlib/xassert.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define PUBLIC_API __attribute__ ((visibility ("default")))
#define UNUSED __attribute__((unused))

#define MSEC_PER_SEC ((hound_data_period) 1e3)
#define NSEC_PER_SEC ((hound_data_period) 1e9)
#define USEC_PER_SEC ((hound_data_period) 1e6)

#define NULL_CHECK(x) \
    do { \
        if (x == NULL) { \
            return HOUND_NULL_VAL; \
        } \
    } while (0);

size_t min(size_t a, size_t b);
size_t max(size_t a, size_t b);

hound_err norm_path(const char *base, const char *path, size_t len, char *out);

void destroy_rq_list(struct hound_data_rq_list *rq_list);

#endif /* HOUND_PRIVATE_UTIL_H_ */
