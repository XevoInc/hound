/**
 * @file      driver_ops.c
 * @brief     Wrappers for driver operations, to ensure safe/correct handling of
 *            driver contexts. All driver operations should be called through
 *            these wrappers rather than directly.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound_private/driver.h>
#include <hound_private/driver_ops.h>
#include <hound_private/error.h>
#include <pthread.h>

/**
 * This thread-local key points to the driver struct for whatever driver is
 * actively processing a callback. We can use it to get and set the driver
 * context without having to explicitly pass a void *ctx to each driver
 * callback. Thus it is very important to reset this pointer prior to calling
 * any driver callback, which should be done using the DRV_OP macro.
 */
static pthread_key_t active_drv;

void set_active_drv(const struct driver *drv)
{
    int ret;

    ret = pthread_setspecific(active_drv, drv);
    XASSERT_EQ(ret, 0);
}

void *get_active_drv(void)
{
    return pthread_getspecific(active_drv);
}


void driver_ops_init(void)
{
    int ret;

    ret = pthread_key_create(&active_drv, NULL);
    XASSERT_EQ(ret, 0);
}

void driver_ops_destroy(void)
{
    int ret;

    ret = pthread_key_delete(active_drv);
    XASSERT_EQ(ret, 0);
}
