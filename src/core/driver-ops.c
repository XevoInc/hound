/**
 * @file      driver-ops.c
 * @brief     Wrappers for driver operations, to ensure safe/correct handling of
 *            driver contexts. All driver operations should be called through
 *            these wrappers rather than directly.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound-private/driver.h>
#include <hound-private/driver-ops.h>
#include <hound-private/error.h>

/**
 * This thread-local key points to the driver struct for whatever driver is
 * actively processing a callback. We can use it to get and set the driver
 * context without having to explicitly pass a void *ctx to each driver
 * callback. Thus it is very important to reset this pointer prior to calling
 * any driver callback, which should be done using the DRV_OP macro.
 */
static _Thread_local struct driver *s_active_drv;

void set_active_drv(struct driver *drv)
{
    XASSERT_NULL(s_active_drv);
    s_active_drv = drv;
}

void clear_active_drv(void)
{
    s_active_drv = NULL;
}

struct driver *get_active_drv(void)
{
    XASSERT_NOT_NULL(s_active_drv);
    return s_active_drv;
}
