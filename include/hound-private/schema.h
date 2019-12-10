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

struct hound_schema_desc {
    hound_data_id data_id;
    char *name;
    size_t fmt_count;
    struct hound_data_fmt *fmts;
};

hound_err schema_parse(
    const char *schema_base,
    const char *schema,
    size_t *out_desc_count,
    struct hound_schema_desc **out_descs);

#endif /* HOUND_PRIVATE_SCHEMA_H_ */
