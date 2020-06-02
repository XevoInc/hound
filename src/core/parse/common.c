/**
 * @file      common.c
 * @brief     Common parsing routines.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2020 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound-private/util.h>
#include <string.h>
#include <xlib/xassert.h>

hound_type parse_type(const char *val)
{
    size_t i;
    static const char *type_strs[] = {
        [HOUND_TYPE_BOOL] = "bool",
        [HOUND_TYPE_BYTES] = "bytes",
        [HOUND_TYPE_DOUBLE] = "double",
        [HOUND_TYPE_FLOAT] = "float",
        [HOUND_TYPE_INT16] = "int16",
        [HOUND_TYPE_INT32] = "int32",
        [HOUND_TYPE_INT64] = "int64",
        [HOUND_TYPE_INT8] = "int8",
        [HOUND_TYPE_UINT16] = "uint16",
        [HOUND_TYPE_UINT32] = "uint32",
        [HOUND_TYPE_UINT64] = "uint64",
        [HOUND_TYPE_UINT8] = "uint8",
    };

    for (i = 0; i < ARRAYLEN(type_strs); ++i) {
        if (strcmp(val, type_strs[i]) == 0) {
            return i;
        }
    }

    /*
     * An unknown type was encountered. Either the schema validator failed, or
     * we need to add a new enum to hound_unit and to the cases list here.
     */
    XASSERT_ERROR;
}
