/**
 * @file      drivers.c
 * @brief     Hound library entrypoint.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound_private/driver.h>
#include <hound_private/io.h>
#include <hound_private/log.h>

__attribute__((constructor))
static void lib_init(void)
{
    log_init();
    io_init();
    driver_init();
}

__attribute__((destructor))
static void lib_destroy(void)
{
    log_destroy();
    io_destroy();
    driver_destroy();
}
