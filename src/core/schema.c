/**
 * @file      schema.c
 * @brief     Schema parser implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/error.h>
#include <hound-private/driver.h>
#include <hound-private/driver/util.h>
#include <hound-private/schema.h>
#include <hound-private/util.h>
#include <linux/limits.h>
#include <pthread.h>
#include <string.h>
#include <yaml.h>

#define MAX_FMT_ENTRIES 100

void destroy_desc_fmts(size_t count, struct hound_data_fmt *fmts)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        drv_free((char *) fmts[i].name);
    }
    drv_free(fmts);
}

void destroy_schema_desc(struct schema_desc *desc)
{
    drv_free((char *) desc->name);
    destroy_desc_fmts(desc->fmt_count, desc->fmts);
}

static
hound_data_id parse_id(yaml_node_t *node)
{
    hound_data_id id;

    XASSERT_EQ(node->type, YAML_SCALAR_NODE);
    XASSERT_GT(node->data.scalar.length, 0);

    errno = 0;
    id = strtol((const char *) node->data.scalar.value, NULL, 0);
    XASSERT_OK(errno);

    return id;
}

static
hound_unit find_unit(const char *val)
{
    if (strcmp(val, "degree") == 0) {
        return HOUND_UNIT_DEGREE;
    }
    else if (strcmp(val, "K") == 0) {
        return HOUND_UNIT_KELVIN;
    }
    else if (strcmp(val, "kg/s") == 0) {
        return HOUND_UNIT_KG_PER_S;
    }
    else if (strcmp(val, "m") == 0) {
        return HOUND_UNIT_METER;
    }
    else if (strcmp(val, "m/s") == 0) {
        return HOUND_UNIT_METERS_PER_S;
    }
    else if (strcmp(val, "m/s^2") == 0) {
        return HOUND_UNIT_METERS_PER_S_SQUARED;
    }
    else if (strcmp(val, "none") == 0) {
        return HOUND_UNIT_NONE;
    }
    else if (strcmp(val, "Pa") == 0) {
        return HOUND_UNIT_PASCAL;
    }
    else if (strcmp(val, "percent") == 0) {
        return HOUND_UNIT_PERCENT;
    }
    else if (strcmp(val, "rad") == 0) {
        return HOUND_UNIT_RAD;
    }
    else if (strcmp(val, "rad/s") == 0) {
        return HOUND_UNIT_RAD_PER_S;
    }
    else if (strcmp(val, "ns") == 0) {
        return HOUND_UNIT_NANOSECOND;
    }
    else {
        /*
         * An unknown type was encountered. Either the schema validator failed, or
         * we need to add a new enum to hound_unit and to the cases list here.
         */
        XASSERT_ERROR;
    }
}

static
hound_type find_type(const char *val)
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

static
const char *parse_str(yaml_node_t *node)
{
    XASSERT_EQ(node->type, YAML_SCALAR_NODE);
    XASSERT_GT(node->data.scalar.length, 0);

    return drv_strdup((const char *) node->data.scalar.value);
}

static
hound_err parse_fmt(
    yaml_document_t *doc,
    yaml_node_t *node,
    struct hound_data_fmt *fmt)
{
    yaml_node_t *key;
    const char *key_str;
    yaml_node_pair_t *pair;
    yaml_node_t *value;

    XASSERT_EQ(node->type, YAML_MAPPING_NODE);
    for (pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top;
         ++pair) {
        key = yaml_document_get_node(doc, pair->key);
        XASSERT_NOT_NULL(key);
        XASSERT_EQ(key->type, YAML_SCALAR_NODE);
        key_str = (const char *) key->data.scalar.value;

        value = yaml_document_get_node(doc, pair->value);
        XASSERT_NOT_NULL(value);

        if (strcmp(key_str, "name") == 0) {
            fmt->name = parse_str(value);
            if (fmt->name == NULL) {
                return HOUND_OOM;
            }

        }
        else if (strcmp(key_str, "unit") == 0) {
            XASSERT_EQ(value->type, YAML_SCALAR_NODE);
            XASSERT_GT(value->data.scalar.length, 0);
            fmt->unit = find_unit((const char *) value->data.scalar.value);
        }
        else if (strcmp(key_str, "type") == 0) {
            XASSERT_EQ(value->type, YAML_SCALAR_NODE);
            XASSERT_GT(value->data.scalar.length, 0);
            fmt->type = find_type((const char *) value->data.scalar.value);

        }
        else {
            XASSERT_ERROR;
        }
    }

    return HOUND_OK;
}

static
hound_err parse_fmts(
    yaml_document_t *doc,
    yaml_node_t *node,
    size_t *out_count,
    struct hound_data_fmt **out_fmts)
{
    size_t fmt_count;
    hound_err err;
    struct hound_data_fmt *fmt;
    struct hound_data_fmt *fmts;
    size_t i;
    yaml_node_item_t *item;

    XASSERT_EQ(node->type, YAML_SEQUENCE_NODE);

    XASSERT_GTE(node->data.sequence.items.top, node->data.sequence.items.start);
    fmt_count = node->data.sequence.items.top - node->data.sequence.items.start;
    XASSERT_LTE(fmt_count, MAX_FMT_ENTRIES);

    fmts = drv_alloc(fmt_count * sizeof(*fmts));
    if (fmts == NULL) {
        return HOUND_OOM;
    }
    *out_fmts = fmts;

    for (item = node->data.sequence.items.start, i = 0;
         item < node->data.sequence.items.top;
         ++item, ++i) {
        fmt = &fmts[i];
        fmt->name = NULL;
        err = parse_fmt(doc, yaml_document_get_node(doc, *item), fmt);
        if (err != HOUND_OK) {
            *out_count = i+1;
            return err;
        }
    }

    *out_count = fmt_count;

    return HOUND_OK;
}

static
hound_err parse_desc(
    yaml_document_t *doc,
    yaml_node_t *node,
    struct schema_desc *desc)
{
    hound_err err;
    yaml_node_t *key;
    const char *key_str;
    yaml_node_pair_t *pair;
    yaml_node_t *value;
    const char *value_str;

    XASSERT_EQ(node->type, YAML_MAPPING_NODE);
    for (pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top;
         ++pair) {
        key = yaml_document_get_node(doc, pair->key);
        XASSERT_NOT_NULL(key);
        XASSERT_EQ(key->type, YAML_SCALAR_NODE);
        key_str = (const char *) key->data.scalar.value;

        value = yaml_document_get_node(doc, pair->value);
        XASSERT_NOT_NULL(value);

        if (strcmp(key_str, "id") == 0) {
            desc->data_id = parse_id(value);
        }
        else if (strcmp(key_str, "name") == 0) {
            XASSERT_EQ(value->type, YAML_SCALAR_NODE);
            value_str = (const char *) value->data.scalar.value;
            desc->name = drv_strdup(value_str);
            if (desc->name == NULL) {
                return HOUND_OOM;
            }
        }
        else if (strcmp(key_str, "fmt") == 0) {
            err = parse_fmts(doc, value, &desc->fmt_count, &desc->fmts);
            if (err != HOUND_OK) {
                return err;
            }
        }
        else {
            XASSERT_ERROR;
        }
    }

    return HOUND_OK;
}

static
hound_err parse_data(
    yaml_document_t *doc,
    yaml_node_t *node,
    size_t *out_desc_count,
    struct schema_desc **out_descs)
{
    size_t desc_count;
    hound_err err;
    struct schema_desc *desc;
    struct schema_desc *descs;
    size_t i;
    yaml_node_item_t *item;

    XASSERT_EQ(node->type, YAML_SEQUENCE_NODE);
    XASSERT_GTE(node->data.sequence.items.top, node->data.sequence.items.start);
    desc_count =
        node->data.sequence.items.top - node->data.sequence.items.start;
    descs = drv_alloc(desc_count * sizeof(*descs));
    if (descs == NULL) {
        return HOUND_OOM;
    }

    err = HOUND_OK;
    for (item = node->data.sequence.items.start, i = 0;
         item < node->data.sequence.items.top;
         ++item, ++i) {
        /* nullify these values so they are safe to cleanup if we have to bail. */
        desc = &descs[i];
        desc->name = NULL;
        desc->fmt_count = 0;
        desc->fmts = NULL;
        err = parse_desc(doc, yaml_document_get_node(doc, *item), desc);
        if (err != HOUND_OK) {
            *out_desc_count = i+1;
            return err;
        }
    }

    *out_desc_count = desc_count;
    *out_descs = descs;

    return err;
}

static
hound_err parse_init(
    yaml_document_t *doc,
    yaml_node_t *node,
    size_t *out_type_count,
    hound_type **out_types)
{
    size_t i;
    yaml_node_item_t *item;
    size_t type_count;
    hound_type *types;
    yaml_node_t *val;

    XASSERT_EQ(node->type, YAML_SEQUENCE_NODE);
    type_count =
        node->data.sequence.items.top - node->data.sequence.items.start;
    XASSERT_GTE(type_count, 1);

    types = drv_alloc(type_count * sizeof(*types));
    if (types == NULL) {
        return HOUND_OOM;
    }

    for (item = node->data.sequence.items.start, i = 0;
         item < node->data.sequence.items.top;
         ++item, ++i) {
        val = yaml_document_get_node(doc, *item);
        XASSERT_NOT_NULL(val);
        XASSERT_EQ(val->type, YAML_SCALAR_NODE);
        types[i] = find_type((const char *) val->data.scalar.value);
    }

    *out_type_count = type_count;
    *out_types = types;

    return HOUND_OK;
}

static
hound_err parse_doc(
    yaml_document_t *doc,
    yaml_node_t *node,
    struct schema_info *info)
{
    hound_err err;
    yaml_node_t *key;
    const char *key_str;
    yaml_node_pair_t *pair;
    yaml_node_t *value;

    XASSERT_EQ(node->type, YAML_MAPPING_NODE);
    err = HOUND_OK;
    for (pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top;
         ++pair) {
        key = yaml_document_get_node(doc, pair->key);
        XASSERT_NOT_NULL(key);
        XASSERT_EQ(key->type, YAML_SCALAR_NODE);
        key_str = (const char *) key->data.scalar.value;

        value = yaml_document_get_node(doc, pair->value);
        if (strcmp(key_str, "init") == 0) {
            err = parse_init(doc, value, &info->type_count, &info->init_types);
            if (err != HOUND_OK) {
                return err;
            }
        }
        else if (strcmp(key_str, "data") == 0) {
            err = parse_data(doc, value, &info->desc_count, &info->descs);
            if (err != HOUND_OK) {
                return err;
            }
        }
    }

    return HOUND_OK;
}

hound_err parse(FILE *file, struct schema_info *info)
{
    yaml_document_t doc;
    hound_err err;
    yaml_node_t *node;
    yaml_parser_t parser;
    int ret;

    ret = yaml_parser_initialize(&parser);
    if (ret == 0) {
        return HOUND_OOM;
    }
    yaml_parser_set_input_file(&parser, file);

    ret = yaml_parser_load(&parser, &doc);
    XASSERT_NEQ(ret, 0);

    node = yaml_document_get_root_node(&doc);
    XASSERT_NOT_NULL(node);

    err = parse_doc(
        &doc,
        node,
        out_type_count,
        out_init_types,
        out_desc_count,
        out_descs);
    yaml_document_delete(&doc);
    /* We should have only one document. */
    XASSERT_NULL(yaml_document_get_root_node(&doc));
    yaml_parser_delete(&parser);
    if (err != HOUND_OK) {
        return err;
    }

    return err;
}

hound_err schema_parse(
    const char *schema_base,
    const char *schema,
    struct schema_info *info)
{
    hound_err err;
    size_t desc_count;
    struct schema_desc *descs;
    FILE *f;
    size_t i;
    hound_type *init_types;
    char path[PATH_MAX];
    size_t type_count;

    XASSERT_NOT_NULL(schema);

    sprintf(path, "%s/%s", schema_base, schema);
    f = fopen(path, "r");
    if (f == NULL) {
        err = HOUND_IO_ERROR;
        goto out;
    }

    memset(info, 0, sizeof(*info));
    err = parse(f, info);
    if (err != HOUND_OK) {
        drv_free(init_types);
        for (i = 0; i < desc_count; ++i) {
            destroy_schema_desc(&descs[i]);
        }
        drv_free(descs);
    }
    fclose(f);

out:
    return err;
}
