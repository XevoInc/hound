/**
 * @file      schema.c
 * @brief     Schema parser implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <hound/hound.h>
#include <hound_private/driver.h>
#include <hound_private/error.h>
#include <hound_private/driver/util.h>
#include <hound_private/util.h>
#include <pthread.h>
#include <string.h>
#include <xlib/xhash.h>
#include <yaml.h>

#define MAX_FMT_ENTRIES 100

XHASH_MAP_INIT_STR(UNIT_NAME_MAP, hound_unit)
XHASH_MAP_INIT_INT8(UNIT_MAP, const char *)

static pthread_mutex_t s_unit_map_lock = PTHREAD_MUTEX_INITIALIZER;
static xhash_t(UNIT_MAP) *s_unit_map = NULL;

typedef enum {
    MAP_NONE,
    MAP_ROOT,
    MAP_FMT
} parse_map;

typedef enum {
    KEY_NAME,
    KEY_UNIT,
    KEY_TYPE,
    KEY_OFFSET,
    KEY_LEN,
    KEY_NONE
} parse_key;

struct parse_state {
    parse_map map;
    parse_key key;
};

void schema_init(void)
{
    s_unit_map = xh_init(UNIT_MAP);
    XASSERT_NOT_NULL(s_unit_map);
}

void schema_destroy(void)
{
    const char *unit_str;

    xh_foreach_value(s_unit_map, unit_str,
        drv_free((void *) unit_str);
    );
    xh_destroy(UNIT_MAP, s_unit_map);
}

hound_err schema_get_unit_str(hound_unit unit, const char **unit_str)
{
    hound_err err;
    xhiter_t iter;

    NULL_CHECK(unit_str);

    pthread_mutex_lock(&s_unit_map_lock);
    iter = xh_get(UNIT_MAP, s_unit_map, unit);
    if (iter == xh_end(s_unit_map)) {
        err = HOUND_UNKNOWN_UNIT;
        goto out;
    }

    err = HOUND_OK;
    *unit_str = xh_val(s_unit_map, iter);

out:
    pthread_mutex_unlock(&s_unit_map_lock);
    return err;
}

static
void free_format_list(struct hound_data_fmt *fmt_list, size_t len)
{
    struct hound_data_fmt *fmt;
    size_t i;

    if (fmt_list == NULL) {
        return;
    }

    for (i = 0; i < len; ++i) {
        fmt = &fmt_list[i];
        drv_free((void *) fmt->name);
        drv_free((void *) fmt->desc);
    }

    drv_free(fmt_list);
}

static
int offset_cmp(const void *p1, const void *p2)
{
    const struct hound_data_fmt *a;
    const struct hound_data_fmt *b;

    a = p1;
    b = p2;

    if (a->offset < b->offset) {
        return -1;
    }
    else if (a->offset == b->offset) {
        return 0;
    }
    else {
        return 1;
    }
}

static
parse_key find_type(const char *val)
{
    if (strcmp(val, "int8") == 0) {
        return HOUND_INT8;
    }
    else if (strcmp(val, "uint8") == 0) {
        return HOUND_UINT8;
    }
    else if (strcmp(val, "int16") == 0) {
        return HOUND_INT16;
    }
    else if (strcmp(val, "uint16") == 0) {
        return HOUND_UINT16;
    }
    else if (strcmp(val, "int32") == 0) {
        return HOUND_INT32;
    }
    else if (strcmp(val, "uint32") == 0) {
        return HOUND_UINT32;
    }
    else if (strcmp(val, "int64") == 0) {
        return HOUND_INT64;
    }
    else if (strcmp(val, "uint64") == 0) {
        return HOUND_UINT64;
    }
    else if (strcmp(val, "float") == 0) {
        return HOUND_FLOAT;
    }
    else if (strcmp(val, "double") == 0) {
        return HOUND_DOUBLE;
    }
    else if (strcmp(val, "bytes") == 0) {
        return HOUND_BYTES;
    }
    else {
        /*
         * An unknown type was encountered. Either the schema validator failed, or
         * we need to add a new enum to hound_type and the cases list here.
         */
        XASSERT_ERROR;
    }
}

static
parse_key find_key(const char *val)
{
    size_t i;
    /* Make sure this stays in sync with the parse_key enum! */
    static const char *keys[] = {
        [KEY_NAME] = "name",
        [KEY_UNIT] = "unit",
        [KEY_TYPE] = "type",
        [KEY_OFFSET] = "offset",
        [KEY_LEN] = "len",
    };

    for (i = 0; i < ARRAYLEN(keys); ++i) {
        if (strcmp(keys[i], val) == 0) {
            return i;
        }
    }

    XASSERT_ERROR;
}

hound_err parse(FILE *file, size_t *count_out, struct hound_data_fmt **fmt_out)
{
    bool done;
    hound_err err;
    yaml_event_t event;
    struct hound_data_fmt *fmt;
    size_t fmt_count;
    struct hound_data_fmt *fmt_list;
    xhiter_t iter;
    hound_unit next_unit_id;
    int ret;
    yaml_parser_t parser;
    struct parse_state state;
    hound_unit unit_id;
    xhash_t(UNIT_NAME_MAP) *unit_name_map;
    const char *unit_str;
    const char *val;

    unit_name_map = xh_init(UNIT_NAME_MAP);
    if (unit_name_map == NULL) {
        return HOUND_OOM;
    }

    ret = yaml_parser_initialize(&parser);
    if (ret == 0) {
        return HOUND_OOM;
    }
    yaml_parser_set_input_file(&parser, file);

    /*
     * Allocate more entries than we need. We will resize it the array when
     * we're done.
     */
    fmt_list = drv_alloc(MAX_FMT_ENTRIES * sizeof(*fmt_list));
    if (fmt_list == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    state.map = MAP_NONE;
    state.key = KEY_NONE;
    done = false;
    fmt_count = 0;
    fmt = NULL;
    next_unit_id = 0;
    err = HOUND_OK;
    pthread_mutex_lock(&s_unit_map_lock);
    do {
        ret = yaml_parser_parse(&parser, &event);
        if (ret == 0) {
            err = HOUND_OOM;
            break;
        }

        switch (event.type) {
            /* We don't handle these. */
            case YAML_NO_EVENT:
            case YAML_ALIAS_EVENT:
            case YAML_SEQUENCE_START_EVENT:
            case YAML_SEQUENCE_END_EVENT:
                XASSERT_ERROR;
                break;

            case YAML_STREAM_START_EVENT:
            case YAML_DOCUMENT_START_EVENT:
                break;
            case YAML_DOCUMENT_END_EVENT:
            case YAML_STREAM_END_EVENT:
                err = HOUND_OK;
                done = true;
                break;

            /*
             * Start events push us one level down the map stack, while end
             * events pop us one level up the map stack.
             */
            case YAML_MAPPING_START_EVENT:
                switch (state.map) {
                    case MAP_NONE:
                        XASSERT_EQ(state.key, KEY_NONE);
                        state.map = MAP_ROOT;
                        break;
                    case MAP_ROOT:
                        XASSERT_EQ(state.key, KEY_NONE);
                        state.map = MAP_FMT;
                        break;
                    case MAP_FMT:
                        XASSERT_ERROR;
                        break;
                }
                state.key = KEY_NONE;
                break;
            case YAML_MAPPING_END_EVENT:
                XASSERT_EQ(state.key, KEY_NONE);
                switch (state.map) {
                    case MAP_NONE:
                        XASSERT_ERROR;
                        break;
                    case MAP_ROOT:
                        state.map = MAP_NONE;
                        break;
                    case MAP_FMT:
                        state.map = MAP_ROOT;
                        break;
                }
                break;

            case YAML_SCALAR_EVENT:
                val = (const char *) event.data.scalar.value;
                switch (state.key) {
                    case KEY_NONE:
                        switch (state.map) {
                            case MAP_NONE:
                                XASSERT_ERROR;
                            case MAP_ROOT:
                                fmt = &fmt_list[fmt_count];
                                ++fmt_count;

                                fmt->name = drv_strdup(val);
                                if (fmt->name == NULL) {
                                    err = HOUND_OOM;
                                    break;
                                }
                                break;
                            case MAP_FMT:
                                state.key = find_key(val);
                                break;
                        }
                        break;

                    case KEY_NAME:
                        XASSERT_EQ(state.map, MAP_FMT);
                        state.key = KEY_NONE;

                        XASSERT_NOT_NULL(fmt);

                        fmt->desc = drv_strdup(val);
                        if (fmt->desc == NULL) {
                            err = HOUND_OOM;
                            done = true;
                        }
                        break;

                    case KEY_UNIT:
                        XASSERT_EQ(state.map, MAP_FMT);
                        state.key = KEY_NONE;

                        XASSERT_NOT_NULL(fmt);

                        /* Check if we have seen this unit before. */
                        iter = xh_get(UNIT_NAME_MAP, unit_name_map, val);
                        if (iter != xh_end(unit_name_map)) {
                            /* We've seen this unit before. */
                            fmt->unit = xh_val(unit_name_map, iter);
                            break;
                        }

                        /* This unit is new. */
                        unit_str = drv_strdup(val);
                        if (unit_str == NULL) {
                            err = HOUND_OOM;
                            done = true;
                            break;
                        }

                        /* Get a new unit ID. */
                        unit_id = next_unit_id;
                        ++next_unit_id;

                        /* Finally put the unit into our maps. */
                        iter = xh_put(
                            UNIT_MAP,
                            s_unit_map,
                            unit_id,
                            &ret);
                        if (ret == -1) {
                            err = HOUND_OOM;
                            done = true;
                            break;
                        }
                        xh_val(s_unit_map, iter) = unit_str;

                        iter = xh_put(UNIT_NAME_MAP, unit_name_map, unit_str, &ret);
                        if (ret == -1) {
                            err = HOUND_OOM;
                            done = true;
                            break;
                        }

                        fmt->unit = unit_id;
                        break;

                    case KEY_TYPE:
                        XASSERT_EQ(state.map, MAP_FMT);
                        state.key = KEY_NONE;

                        fmt->type = find_type(val);
                        break;

                    case KEY_OFFSET:
                        XASSERT_EQ(state.map, MAP_FMT);
                        state.key = KEY_NONE;

                        errno = 0;
                        fmt->offset = strtol(val, NULL, 0);
                        XASSERT_EQ(errno, 0);
                        break;

                    case KEY_LEN:
                        XASSERT_EQ(state.map, MAP_FMT);
                        state.key = KEY_NONE;

                        errno = 0;
                        fmt->len = strtol(val, NULL, 0);
                        XASSERT_EQ(errno, 0);
                        break;
                }
                break;
        }
        yaml_event_delete(&event);

    } while (!done);

    /* Trim the format list size to match what we actually found. */
    fmt_list = drv_realloc(fmt_list, fmt_count * sizeof(*fmt_list));
    if (fmt_list == NULL) {
        err = HOUND_OOM;
    }

    if (err == HOUND_OK) {
        qsort(fmt_list, fmt_count, sizeof(*fmt_list), offset_cmp);
    }
    else {
        free_format_list(fmt_list, fmt_count);
        xh_clear(UNIT_MAP, s_unit_map);
        xh_foreach_key(unit_name_map, unit_str,
            drv_free((void *) unit_str);
        );
    }

    pthread_mutex_unlock(&s_unit_map_lock);

    xh_destroy(UNIT_NAME_MAP, unit_name_map);

    *count_out = fmt_count;
    *fmt_out = fmt_list;

out:
    yaml_parser_delete(&parser);
    return err;
}

hound_err schema_parse(
    const char *schema_base,
    const char *schema,
    size_t *fmt_count,
    struct hound_data_fmt **fmt_list)
{
    hound_err err;
    FILE *f;
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

    err = parse(f, fmt_count, fmt_list);
    fclose(f);

out:
    return err;
}
