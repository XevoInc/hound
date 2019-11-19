/**
 * @file      schema.h
 * @brief     Schema subsystem header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_SCHEMA_H_
#define HOUND_PRIVATE_SCHEMA_H_

#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/queue.h>

void schema_init(void);
void schema_destroy(void);

hound_err schema_parse(
    const char *schema_base,
    const char *schema,
    const char **name,
    size_t *fmt_count,
    struct hound_data_fmt **fmt_list);

#endif /* HOUND_PRIVATE_SCHEMA_H_ */
