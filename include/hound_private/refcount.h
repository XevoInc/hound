/**
 * @file      refcount.h
 * @brief     Refcount logic.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_REFCOUNT_H__
#define HOUND_PRIVATE_REFCOUNT_H__

#include <stdint.h>

typedef uint_fast32_t refcount_val;
typedef _Atomic refcount_val atomic_refcount_val;

void atomic_ref_init(atomic_refcount_val *ref, refcount_val val);
refcount_val atomic_ref_inc(atomic_refcount_val *count);
refcount_val atomic_ref_add(atomic_refcount_val *count, atomic_refcount_val val);
refcount_val atomic_ref_dec(atomic_refcount_val *count);

#endif /* HOUND_PRIVATE_REFCOUNT_H__ */
