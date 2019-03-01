/**
 * @file      util.c
 * @brief     Utility code.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound_private/util.h>

size_t min(size_t a, size_t b)
{
    if (a < b) {
        return a;
    }
    else {
        return b;
    }
}

size_t max(size_t a, size_t b)
{
    if (a > b) {
        return a;
    }
    else {
        return b;
    }
}
