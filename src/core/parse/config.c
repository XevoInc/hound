/*
 * @file      config.c
 * @brief     Implementation for config parsing.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2020 Xevo Inc. All Rights Reserved.
 *
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <hound/hound.h>
#include <hound-private/log.h>
#include <hound-private/parse/common.h>
#include <hound-private/util.h>
#include <linux/limits.h>
#include <string.h>
#include <xlib/xassert.h>
#include <yaml.h>

#include "config.h"

struct driver_init {
    const char *name;
    const char *path;
    const char *schema;
    size_t arg_count;
    struct hound_init_arg *args;
};

#define CHECK_ERRNO \
    do { \
        if (errno != 0) { \
            return errno; \
        } \
    } while (0);

#define PARSE_FLOAT(type, parse_func, data, arg) \
    do { \
        errno = 0; \
        arg->data.as_##type = parse_func(data, NULL); \
        CHECK_ERRNO; \
    } while (0);

#define PARSE_INT(type, parse_func, min, max, data, arg) \
    do { \
        errno = 0; \
        arg->data.as_##type = parse_func(data, NULL, 0); \
        CHECK_ERRNO; \
        if (arg->data.as_##type < min || arg->data.as_##type > max) { \
            return HOUND_INVALID_VAL; \
        } \
    } while (0);

#define PARSE_UINT(type, parse_func, max, data, arg) \
    do { \
        errno = 0; \
        arg->data.as_##type = parse_func(data, NULL, 0); \
        CHECK_ERRNO; \
        if (arg->data.as_##type > max) { \
            return HOUND_INVALID_VAL; \
        } \
    } while (0);

static
hound_err populate_arg(
    const char *type_str,
    const char *data,
    struct hound_init_arg *arg)
{
    hound_type type;
    type = parse_type(type_str);
    switch (type) {
        case HOUND_TYPE_FLOAT:
            PARSE_FLOAT(float, strtof, data, arg);
            break;
        case HOUND_TYPE_DOUBLE:
            PARSE_FLOAT(double, strtod, data, arg);
            break;
        case HOUND_TYPE_INT8:
            PARSE_INT(int8, strtoimax, INT8_MIN, INT8_MAX, data, arg);
            break;
        case HOUND_TYPE_INT16:
            PARSE_INT(int16, strtoimax, INT16_MIN, INT16_MAX, data, arg);
            break;
        case HOUND_TYPE_INT32:
            PARSE_INT(int32, strtoimax, INT32_MIN, INT32_MAX, data, arg);
            break;
        case HOUND_TYPE_INT64:
            PARSE_INT(int64, strtoimax, INT64_MIN, INT64_MAX, data, arg);
            break;
        case HOUND_TYPE_UINT8:
            PARSE_UINT(uint8, strtoumax, UINT8_MAX, data, arg);
            break;
        case HOUND_TYPE_UINT16:
            PARSE_UINT(uint16, strtoumax, UINT16_MAX, data, arg);
            break;
        case HOUND_TYPE_UINT32:
            PARSE_UINT(uint32, strtoumax, UINT32_MAX, data, arg);
            break;
        case HOUND_TYPE_UINT64:
            PARSE_UINT(uint64, strtoumax, UINT64_MAX, data, arg);
            break;
        case HOUND_TYPE_BYTES:
            arg->data.as_bytes = data;
            break;
    }

    arg->type = type;

    return HOUND_OK;
}

static
hound_err parse_arg(
    yaml_document_t *doc,
    yaml_node_t *node,
    struct hound_init_arg *arg)
{
    const char *data;
    yaml_node_t *key;
    const char *key_str;
    yaml_node_pair_t *pair;
    const char *type_str;
    yaml_node_t *val;
    const char *val_str;

    XASSERT_EQ(node->type, YAML_MAPPING_NODE);
    for (pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top;
         ++pair) {
        key = yaml_document_get_node(doc, pair->key);
        XASSERT_NOT_NULL(key);
        XASSERT_EQ(key->type, YAML_SCALAR_NODE);
        key_str = (const char *) key->data.scalar.value;

        val = yaml_document_get_node(doc, pair->value);
        XASSERT_EQ(val->type, YAML_SCALAR_NODE);
        val_str = (const char *) val->data.scalar.value;

        if (strcmp(key_str, "type") == 0) {
            type_str = val_str;
        }
        else if (strcmp(key_str, "val") == 0) {
            data = val_str;
        }
    }

    populate_arg(type_str, data, arg);

    return HOUND_OK;
}

static
hound_err parse_args(
    yaml_document_t *doc,
    yaml_node_t *node,
    struct driver_init *init)
{
    yaml_node_t *arg_node;
    size_t arg_count;
    hound_err err;
    size_t i;
    yaml_node_item_t *item;

    if (node->type == YAML_NO_NODE ||
        (node->type == YAML_SCALAR_NODE && node->data.scalar.length == 0)) {
        /* Driver has no arguments. */
        init->arg_count = 0;
        init->args = NULL;
        return HOUND_OK;
    }

    XASSERT_EQ(node->type, YAML_SEQUENCE_NODE);
    XASSERT_GTE(node->data.sequence.items.top, node->data.sequence.items.start);
    arg_count =
        node->data.sequence.items.top - node->data.sequence.items.start;
    init->args = malloc(arg_count * sizeof(*init->args));
    if (init->args == NULL) {
        init->arg_count = 0;
        return HOUND_OOM;
    }

    err = HOUND_OK;
    for (item = node->data.sequence.items.start, i = 0;
         item < node->data.sequence.items.top;
         ++item, ++i) {
        arg_node = yaml_document_get_node(doc, *item);
        XASSERT_NOT_NULL(arg_node);
        err = parse_arg(doc, arg_node, &init->args[i]);
        if (err != HOUND_OK) {
            break;
        }
    }

    init->arg_count = i;

    return err;
}

static
hound_err parse_driver(
    yaml_document_t *doc,
    yaml_node_t *node,
    struct driver_init *init)
{
    hound_err err;
    yaml_node_t *key;
    const char *key_str;
    yaml_node_pair_t *pair;
    yaml_node_t *val;
    const char *val_str;

    XASSERT_EQ(node->type, YAML_MAPPING_NODE);
    for (pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top;
         ++pair) {
        key = yaml_document_get_node(doc, pair->key);
        XASSERT_NOT_NULL(key);
        XASSERT_EQ(key->type, YAML_SCALAR_NODE);
        key_str = (const char *) key->data.scalar.value;

          val = yaml_document_get_node(doc, pair->value);
        if (strcmp(key_str, "name") == 0) {
            XASSERT_EQ(val->type, YAML_SCALAR_NODE);
            val_str = (const char *) val->data.scalar.value;
            init->name = val_str;
        }
        else if (strcmp(key_str, "path") == 0) {
            XASSERT_EQ(val->type, YAML_SCALAR_NODE);
            val_str = (const char *) val->data.scalar.value;
            init->path = val_str;
        }
        else if (strcmp(key_str, "schema") == 0) {
            XASSERT_EQ(val->type, YAML_SCALAR_NODE);
            val_str = (const char *) val->data.scalar.value;
            init->schema = val_str;
        }
        else if (strcmp(key_str, "args") == 0) {
            err = parse_args(doc, val, init);
            if (err != HOUND_OK) {
                return err;
            }
        }
        else {
          /* unknown key */
          XASSERT_ERROR;
        }
    }

    return HOUND_OK;
}

static
int parse_doc(
    yaml_document_t *doc,
    yaml_node_t *node,
    size_t *out_init_count,
    struct driver_init **out_init_list)
{
    yaml_node_t *driver_node;
    hound_err err;
    size_t i;
    struct driver_init *init;
    size_t init_count;
    struct driver_init *init_list;
    yaml_node_item_t *item;

    XASSERT_EQ(node->type, YAML_SEQUENCE_NODE);
    XASSERT_GTE(node->data.sequence.items.top, node->data.sequence.items.start);
    init_count =
        node->data.sequence.items.top - node->data.sequence.items.start;
    init_list = malloc(init_count * sizeof(*init_list));
    if (init_list == NULL) {
        return HOUND_OOM;
    }

    err = HOUND_OK;
    for (item = node->data.sequence.items.start, i = 0;
         item < node->data.sequence.items.top;
         ++item, ++i) {
        init = &init_list[i];
        driver_node = yaml_document_get_node(doc, *item);
        XASSERT_NOT_NULL(driver_node);
        err = parse_driver(doc, driver_node, init);
        if (err != HOUND_OK) {
            break;
        }
    }

    *out_init_count = i;
    *out_init_list = init_list;

    return err;
}

static
hound_err register_drivers(
    size_t init_count,
    struct driver_init *init_list,
    const char *schema_base)
{
    hound_err err;
    hound_err err2;
    size_t i;
    struct driver_init *init;

    for (i = 0; i < init_count; ++i) {
        init = &init_list[i];
        err = hound_init_driver(
            init->name,
            init->path,
            schema_base,
            init->schema,
            init->arg_count,
            init->args);
        if (err != HOUND_OK) {
            for (--i; i < init_count; --i) {
                err2 = hound_destroy_driver(init->path);
                if (err != HOUND_OK) {
                    hound_log_err(
                        err2,
                        "failed to unregister driver %s at path %s",
                        init->name,
                        init->path);
                }
            }
            break;
        }
    }

    return err;
}

static
void destroy_init_list(size_t init_count, struct driver_init *init_list)
{
    size_t i;

    for (i = 0; i < init_count; ++i) {
        free(init_list[i].args);
    }

    free(init_list);
}

static
hound_err register_config(FILE *file, const char *schema_base)
{
    yaml_document_t doc;
    hound_err err;
    size_t init_count;
    struct driver_init *init_list;
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

    /* nullify these so we can still safely cleanup if we have to bail. */
    init_count = 0;
    init_list = NULL;
    err = parse_doc(&doc, node, &init_count, &init_list);
    if (err != HOUND_OK) {
        goto out;
    }

    err = register_drivers(init_count, init_list, schema_base);
    if (err != HOUND_OK) {
        goto out;
    }

out:
    destroy_init_list(init_count, init_list);
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    return err;
}

hound_err parse_config(const char *config_path, const char *schema_base)
{
    hound_err err;
    FILE *f;
    char path[PATH_MAX];

    norm_path(CONFIG_HOUND_CONFDIR, config_path, ARRAYLEN(path), path);
    f = fopen(path, "r");
    if (f == NULL) {
        err = HOUND_IO_ERROR;
        goto error_fopen;
    }

    err = register_config(f, schema_base);
    fclose(f);
    if (err != HOUND_OK) {
        goto error_parse;
    }

    return HOUND_OK;

error_parse:
error_fopen:
    return err;
}
