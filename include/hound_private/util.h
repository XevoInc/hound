/**
 * @file      util.h
 * @brief     Utility functions.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_UTIL_H_
#define HOUND_PRIVATE_UTIL_H_

#include <stddef.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define UNUSED __attribute__((unused))

#define NULL_CHECK(x) \
    do { \
        if (x == NULL) { \
            return HOUND_NULL_VAL; \
        } \
    } while (0);

/*
 * This macro must be used carefully to avoid side-effects. We could use an
 * inline function if we are willing to use void *, or declare separate versions
 * per type. None of these are great, so instead we just trust the caller to
 * behave.
 */
#define SWAP(a, i, j) \
    do { \
        __typeof__(*(a)) __hound_util_tmp; \
        __hound_util_tmp = (a)[(i)]; \
        (a)[(i)] = a[(j)]; \
        (a)[(j)] = __hound_util_tmp; \
    } while (0);

#define RM_VEC_INDEX(v, i) \
    do { \
        SWAP(xv_data(v), i, xv_size(v)-1); \
        (void) xv_pop(v); \
    } while (0);

size_t min(size_t a, size_t b);
size_t max(size_t a, size_t b);

#endif /* HOUND_PRIVATE_UTIL_H_ */
