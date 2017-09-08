/**
 * @file      refcount.c
 * @brief     Refcount logic.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound_private/error.h>
#include <hound_private/refcount.h>
#include <stdatomic.h>

void atomic_ref_init(atomic_refcount_val *count, refcount_val val)
{
    atomic_init(count, val);
}

refcount_val atomic_ref_inc(atomic_refcount_val *ref)
{
    return atomic_ref_add(ref, 1);
}

refcount_val atomic_ref_add(atomic_refcount_val *count, atomic_refcount_val val)
{
    XASSERT_NEQ(count, NULL);
    return atomic_fetch_add_explicit(count, val, memory_order_relaxed);
}

refcount_val atomic_ref_dec(atomic_refcount_val *count)
{
    XASSERT_NEQ(count, NULL);
    XASSERT_GT(*count, 0);
    return atomic_fetch_sub_explicit(count, 1, memory_order_relaxed);
}
