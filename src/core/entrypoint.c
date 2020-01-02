/**
 * @file      entrypoint.c
 * @brief     Hound library entrypoint.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound-private/driver.h>
#include <hound-private/io.h>
#include <hound-private/log.h>
#include <hound-private/parse/schema.h>

#define CORE_PRIO (HOUND_DRIVER_REGISTER_PRIO-1)

__attribute__((constructor(CORE_PRIO)))
static void lib_init(void)
{
    log_init();
    io_init();
    driver_init_statics();
}

__attribute__((destructor))
static void lib_destroy(void)
{
    log_destroy();
    io_destroy();
    driver_destroy_statics();
}
