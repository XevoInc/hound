/**
 * @file      entrypoint.c
 * @brief     Hound library entrypoint.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound-private/driver.h>
#include <hound-private/io.h>
#include <hound-private/log.h>
#include <hound-private/schema.h>

__attribute__((constructor))
static void lib_init(void)
{
    log_init();
    schema_init();
    io_init();
    driver_init();
}

__attribute__((destructor))
static void lib_destroy(void)
{
    log_destroy();
    schema_destroy();
    io_destroy();
    driver_destroy();
}
