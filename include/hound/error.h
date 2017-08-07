/**
 * @file      assert.h
 * @brief     Assertions used by core, drivers, and test code.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 *
 * Note that the assertion code is adapted from the code in Ellis, originally
 * written by Martin Kelly and James Corey. The original file is here:
 *
 * https://github.com/project-ellis/ellis/blob/master/include/ellis/core/system.hpp
 *
 */

#ifndef HOUND_ASSERT_H_
#define HOUND_ASSERT_H_

#include <hound/hound.h>
#include <hound/log.h>
#include <stdlib.h>

void _hound_error_log_msg(
    const char *expr,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    ...);

#define _HOUND_ASSERT_STR_BASE "Assert: failed expression ("
#define _HOUND_ASSERT_STR_LOC_DETAILS ") at %s:%d [%s]"

#define _HOUND_ASSERT_SKELETON(expr, log_code) \
    do { \
      if (expr) { \
        /* Empty, but catches accidental assignment (i.e. a=b) in expr. */ \
      } \
      else { \
        log_code; \
        abort(); \
      } \
    } while (0);

#define _HOUND_ASSERT_FMT(expr, fmt, ...) \
    _HOUND_ASSERT_SKELETON(expr, \
        _hound_error_log_msg( \
            #expr, \
            __FILE__, \
            __LINE__, \
            __func__, \
            "LHS: " fmt "\nRHS: " fmt, \
            __VA_ARGS__));

#define _HOUND_ASSERT_OP_FMT(op, fmt, x, y) _HOUND_ASSERT_FMT(x op y, fmt, x, y)

#define HOUND_ASSERT(expr) \
    _HOUND_ASSERT_SKELETON(expr, \
        hound_log_msg( \
            LOG_CRIT, \
            _HOUND_ASSERT_STR_BASE #expr _HOUND_ASSERT_STR_LOC_DETAILS, \
           __FILE__, \
           __LINE__, \
           __func__));

#define HOUND_ASSERT_LT_FMT(fmt, x, y) _HOUND_ASSERT_OP_FMT(<, fmt, x, y)
#define HOUND_ASSERT_LTE_FMT(fmt, x, y) _HOUND_ASSERT_OP_FMT(<=, fmt, x, y)
#define HOUND_ASSERT_EQ_FMT(fmt, x, y) _HOUND_ASSERT_OP_FMT(==, fmt, x, y)
#define HOUND_ASSERT_NEQ_FMT(fmt, x, y) _HOUND_ASSERT_OP_FMT(!=, fmt, x, y)
#define HOUND_ASSERT_GT_FMT(fmt, x, y) _HOUND_ASSERT_OP_FMT(>, fmt, x, y)
#define HOUND_ASSERT_GTE_FMT(fmt, x, y) _HOUND_ASSERT_OP_FMT(>=, fmt, x, y)

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)

#include <stdatomic.h>
#include <stdbool.h>

#define _HOUND_FMT(x) _Generic((x), \
    char:                        "LHS: %c\n" \
                                 "RHS: %c", \
    signed char:                 "LHS: %hhd\n" \
                                 "RHS: %hhd", \
    unsigned char:               "LHS: %hhu\n" \
                                 "RHS: %hhu", \
    signed short:                "LHS: %hd\n" \
                                 "RHS: %hd", \
    unsigned short:              "LHS: %hu\n" \
                                 "RHS: %hu", \
    signed int:                  "LHS: %d\n" \
                                 "RHS: %d", \
    unsigned int:                "LHS: %u\n" \
                                 "RHS: %u", \
    long int:                    "LHS: %ld\n" \
                                 "RHS: %ld", \
    unsigned long int:           "LHS: %lu\n" \
                                 "RHS: %lu", \
    _Atomic long unsigned int *: "RHS: %lu" \
                                 "LHS: %lu", \
    long long int:               "LHS: %lld\n" \
                                 "RHS: %lld", \
    unsigned long long int:      "LHS: %llu\n" \
                                 "RHS: %llu", \
    float:                       "LHS: %f\n" \
                                 "RHS: %f", \
    double:                      "LHS: %f\n" \
                                 "RHS: %f", \
    long double:                 "LHS: %f\n" \
                                 "RHS: %f", \
    char *:                      "LHS: %s\n" \
                                 "RHS: %s", \
    void *:                      "LHS: %p\n" \
                                 "RHS: %p" \
    )

#define _HOUND_ASSERT_GENERIC(expr, x, y) \
    _HOUND_ASSERT_SKELETON(expr, \
        _hound_error_log_msg(#expr, __FILE__, __LINE__, __func__, _HOUND_FMT(x), x, y));

#define _HOUND_ASSERT_OP_GENERIC(op, x, y) _HOUND_ASSERT_GENERIC(x op y, x, y)

#define HOUND_ASSERT_LT(x, y) _HOUND_ASSERT_OP_GENERIC(<, x, y)
#define HOUND_ASSERT_LTE(x, y) _HOUND_ASSERT_OP_GENERIC(<=, x, y)
#define HOUND_ASSERT_EQ(x, y) _HOUND_ASSERT_OP_GENERIC(==, x, y)
#define HOUND_ASSERT_NEQ(x, y) _HOUND_ASSERT_OP_GENERIC(!=, x, y)
#define HOUND_ASSERT_GT(x, y) _HOUND_ASSERT_OP_GENERIC(>, x, y)
#define HOUND_ASSERT_GTE(x, y) _HOUND_ASSERT_OP_GENERIC(>=, x, y)

#define HOUND_ASSERT_NULL(x) HOUND_ASSERT_EQ((void *) (x), NULL)
#define HOUND_ASSERT_NOT_NULL(x) HOUND_ASSERT_NEQ((void *) (x), NULL)

#else

#define HOUND_ASSERT_LT(fmt, x, y) HOUND_ASSERT_LT_FMT(fmt, x, y)
#define HOUND_ASSERT_LTE(fmt, x, y) HOUND_ASSERT_LTE_FMT(fmt, x, y)
#define HOUND_ASSERT_EQ(fmt, x, y) HOUND_ASSERT_EQ_FMT(fmt, x, y)
#define HOUND_ASSERT_NEQ(fmt, x, y) HOUND_ASSERT_NEQ_FMT(fmt, x, y)
#define HOUND_ASSERT_GT(fmt, x, y) HOUND_ASSERT_GT_FMT(fmt, x, y)
#define HOUND_ASSERT_GTE(fmt, x, y) HOUND_ASSERT_GTE_FMT(fmt, x, y)

#define HOUND_ASSERT_NULL(x) HOUND_ASSERT_EQ(%p, x, NULL)
#define HOUND_ASSERT_NOT_NULL(x) HOUND_ASSERT_NEQ(%p, x, NULL)

#endif /* defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) */

#define HOUND_ASSERT_TRUE(x) HOUND_ASSERT(x)
#define HOUND_ASSERT_FALSE(x) HOUND_ASSERT(!(x))
#define HOUND_ASSERT_ERROR HOUND_ASSERT_TRUE(0)

#define HOUND_ASSERT_ERRCODE(x, y) \
    _HOUND_ASSERT_FMT(x == y, "%d (%s)", x, hound_strerror(x), y, hound_strerror(y))
#define HOUND_ASSERT_OK(err) HOUND_ASSERT_ERRCODE(err, HOUND_OK)

#endif /* HOUND_PRIVATE_ASSERT_H_ */
