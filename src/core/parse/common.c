/**
 * @file      common.c
 * @brief     Common parsing routines.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2020 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <string.h>
#include <xlib/xassert.h>

hound_type parse_type(const char *val)
{
    if (strcmp(val, "int8") == 0) {
        return HOUND_TYPE_INT8;
    }
    else if (strcmp(val, "uint8") == 0) {
        return HOUND_TYPE_UINT8;
    }
    else if (strcmp(val, "int16") == 0) {
        return HOUND_TYPE_INT16;
    }
    else if (strcmp(val, "uint16") == 0) {
        return HOUND_TYPE_UINT16;
    }
    else if (strcmp(val, "int32") == 0) {
        return HOUND_TYPE_INT32;
    }
    else if (strcmp(val, "uint32") == 0) {
        return HOUND_TYPE_UINT32;
    }
    else if (strcmp(val, "int64") == 0) {
        return HOUND_TYPE_INT64;
    }
    else if (strcmp(val, "uint64") == 0) {
        return HOUND_TYPE_UINT64;
    }
    else if (strcmp(val, "float") == 0) {
        return HOUND_TYPE_FLOAT;
    }
    else if (strcmp(val, "double") == 0) {
        return HOUND_TYPE_DOUBLE;
    }
    else if (strcmp(val, "bytes") == 0) {
        return HOUND_TYPE_BYTES;
    }
    else {
        /*
         * An unknown type was encountered. Either the schema validator failed, or
         * we need to add a new enum to hound_type and to the cases list here.
         */
        XASSERT_ERROR;
    }
}
