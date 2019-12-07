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
#include <xlib/xvec.h>
#include <yaml.h>

#define MAX_FMT_ENTRIES 100

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
    hound_data_count *out_count,
    struct hound_data_fmt **out_fmts)
{
    size_t fmt_count;
    hound_err err;
    struct hound_data_fmt *fmts;
    size_t i;
    yaml_node_item_t *item;

    XASSERT_EQ(node->type, YAML_SEQUENCE_NODE);

    fmt_count =
        node->data.sequence.items.top - node->data.sequence.items.start;
    XASSERT_GTE(fmt_count, 1);
    XASSERT_LTE(fmt_count, MAX_FMT_ENTRIES);

    fmts = drv_alloc(fmt_count * sizeof(*fmts));
    if (fmts == NULL) {
        return HOUND_OOM;
    }

    for (item = node->data.sequence.items.start, i = 0;
         item < node->data.sequence.items.top;
         ++item, ++i) {
        err = parse_fmt(doc, yaml_document_get_node(doc, *item), &fmts[i]);
        if (err != HOUND_OK) {
            *out_count = i;
            return err;
        }
    }

    *out_count = fmt_count;
    *out_fmts = fmts;

    return HOUND_OK;
}

static
hound_err parse_doc(
    yaml_document_t *doc,
    yaml_node_t *node,
    struct hound_schema_desc *desc)
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

hound_err parse(
    FILE *file,
    hound_data_count *out_desc_count,
    struct hound_schema_desc **out_descs)
{
    struct hound_schema_desc *desc;
    xvec_t(struct hound_schema_desc) descs;
    size_t descs_size;
    yaml_document_t doc;
    hound_err err;
    size_t i;
    size_t j;
    yaml_node_t *node;
    yaml_parser_t parser;
    int ret;

    ret = yaml_parser_initialize(&parser);
    if (ret == 0) {
        return HOUND_OOM;
    }
    yaml_parser_set_input_file(&parser, file);

    err = HOUND_OK;
    xv_init(descs);
    while (true) {
        ret = yaml_parser_load(&parser, &doc);
        XASSERT_NEQ(ret, 0);

        node = yaml_document_get_root_node(&doc);
        if (node == NULL) {
            /* End of stream. */
            break;
        }

        desc = xv_pushp(struct hound_schema_desc, descs);
        if (desc == NULL) {
            err = HOUND_OOM;
            break;
        }
        /* NULL this out so that if we fail, we can call drv_free on members. */
        desc->name = NULL;
        desc->fmt_count = 0;
        desc->fmts = NULL;

        err = parse_doc(&doc, node, desc);
        if (err != HOUND_OK) {
            break;
        }

        yaml_document_delete(&doc);
    }
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);

    if (err == HOUND_OK) {
        descs_size = xv_size(descs) * sizeof(**out_descs);
        *out_descs = drv_alloc(descs_size);
        if (*out_descs == NULL) {
            err = HOUND_OOM;
        }
        else {
            memcpy(*out_descs, xv_data(descs), descs_size);
            *out_desc_count = xv_size(descs);
        }
    }

    if (err != HOUND_OK) {
        for (i = 0; i < xv_size(descs); ++i) {
            desc = &xv_A(descs, i);
            drv_free(desc->name);
            for (j = 0; j < desc->fmt_count; ++j) {
                drv_free((char *) desc->fmts[j].name);
            }
            drv_free(desc->fmts);
        }
    }
    xv_destroy(descs);

    return err;
}

hound_err schema_parse(
    const char *schema_base,
    const char *schema,
    hound_data_count *out_desc_count,
    struct hound_schema_desc **out_descs)
{
    hound_err err;
    FILE *f;
    hound_data_count desc_count;
    struct hound_schema_desc *descs;
    size_t len;
    size_t total_len;
    char *path;

    XASSERT_NOT_NULL(schema);

    total_len = 0;
    len = strnlen(schema_base, PATH_MAX);
    XASSERT_NEQ(len, PATH_MAX);
    total_len += len;

    len = strnlen(schema, PATH_MAX);
    XASSERT_NEQ(len, PATH_MAX);
    /*
     * len --> schema dir length
     * + 1 --> '/' to join paths
     * len --> length of path relative to schema dir
     * + 1 --> '\0'
     */
    total_len += len + 2;
    XASSERT_LTE(total_len, PATH_MAX);

    path = drv_alloc(total_len);
    if (path == NULL) {
        return HOUND_OOM;
    }
    sprintf(path, "%s/%s", schema_base, schema);

    f = fopen(path, "r");
    drv_free(path);

    if (f == NULL) {
        err = HOUND_IO_ERROR;
        goto out;
    }

    err = parse(f, &desc_count, &descs);
    fclose(f);
    if (err == HOUND_OK) {
        *out_desc_count = desc_count;
        *out_descs = descs;
    }

out:
    return err;
}
