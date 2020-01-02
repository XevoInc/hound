/**
 * @file      schema.h
 * @brief     Header for schema parsing routines.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_PARSE_SCHEMA_H_
#define HOUND_PRIVATE_PARSE_SCHEMA_H_

#include <hound/hound.h>

void schema_init(void);
void schema_destroy(void);

struct schema_desc {
    hound_data_id data_id;
    char *name;
    size_t fmt_count;
    struct hound_data_fmt *fmts;
};

void destroy_desc_fmts(size_t count, struct hound_data_fmt *fmts);
void destroy_schema_desc(struct schema_desc *desc);

hound_err schema_parse(
    const char *schema_base,
    const char *schema,
    size_t *out_desc_count,
    struct schema_desc **out_descs);

#endif /* HOUND_PRIVATE_PARSE_SCHEMA_H_ */
