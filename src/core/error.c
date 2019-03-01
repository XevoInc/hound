/**
 * @file      error.c
 * @brief     Hound error handling functions.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound_private/error.h>
#include <hound_private/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

const char *error_strerror(hound_err err)
{
    /* Not a hound error code. */
    if (err > 0) {
        return strerror(err);
    }

    switch (err) {
        case HOUND_OK:
            return "OK";
        case HOUND_NULL_VAL:
            return "NULL value specified";
        case HOUND_OOM:
            return "out of memory!";
        case HOUND_DRIVER_ALREADY_REGISTERED:
            return "driver is already registered";
        case HOUND_DRIVER_NOT_REGISTERED:
            return "driver is not registered";
        case HOUND_DRIVER_IN_USE:
            return "driver is in-use";
        case HOUND_MISSING_DEVICE_IDS:
            return "driver specifies NULL device IDs";
        case HOUND_CONFLICTING_DRIVERS:
            return "two drivers registered for the same data ID";
        case HOUND_NO_DATA_REQUESTED:
            return "context does not request any data";
        case HOUND_DATA_ID_DOES_NOT_EXIST:
            return "context requests data ID not registered with a driver";
        case HOUND_CTX_ACTIVE:
            return "context is active";
        case HOUND_CTX_NOT_ACTIVE:
            return "context has not been started";
        case HOUND_EMPTY_QUEUE:
            return "context requests a data queue of length 0";
        case HOUND_MISSING_CALLBACK:
            return "context does not specify a data callback";
        case HOUND_PERIOD_UNSUPPORTED:
            return "context requests a period not supported by the backing driver";
        case HOUND_IO_ERROR:
            return "I/O error (EIO)";
        case HOUND_QUEUE_TOO_SMALL:
            return "blocking read requested for more samples than the max queue size";
        case HOUND_INVALID_STRING:
            return "string is not null-terminated, or is too long";
        case HOUND_DRIVER_UNSUPPORTED:
            return "the driver does not support this request";
        case HOUND_DRIVER_FAIL:
            return "the driver failed to complete the requested operation";
        case HOUND_INVALID_VAL:
            return "value specified is invalid";
        case HOUND_INTR:
            return "operation was interrupted";
        case HOUND_DEV_DOES_NOT_EXIST:
            return "the given device ID does not exist";
        case HOUND_UNKNOWN_UNIT:
            return "the given unit ID does not exist";
    }

    /*
     * The user passed in something that wasn't a valid error code. We could
     * could make this the default case, but then the compiler would not be able
     * to check that we covered all valid error cases.
     */
    return NULL;
}
