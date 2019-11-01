/**
 * @file      hound.c
 * @brief     Hound public implementation, gluing together all subsystems.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound-private/api.h>
#include <hound-private/ctx.h>
#include <hound-private/error.h>
#include <hound-private/driver.h>
#include <hound-private/log.h>
#include <hound-private/schema.h>

PUBLIC_API
hound_err hound_get_dev_name(hound_dev_id id, const char **name)
{
    return driver_get_dev_name(id, name);
}

PUBLIC_API
hound_err hound_get_unit_str(hound_unit unit, const char **unit_str)
{
    return schema_get_unit_str(unit, unit_str);
}

PUBLIC_API
hound_err hound_get_datadesc(struct hound_datadesc **desc, size_t *len)
{
    return driver_get_datadesc(desc, len);
}

PUBLIC_API
void hound_free_datadesc(struct hound_datadesc *desc)
{
    driver_free_datadesc(desc);
}

PUBLIC_API
hound_err hound_alloc_ctx(struct hound_ctx **ctx, const struct hound_rq *rq)
{
    return ctx_alloc(ctx, rq);
}

PUBLIC_API
hound_err hound_free_ctx(struct hound_ctx *ctx)
{
    return ctx_free(ctx);
}

PUBLIC_API
hound_err hound_start(struct hound_ctx *ctx)
{
    return ctx_start(ctx);
}

PUBLIC_API
hound_err hound_stop(struct hound_ctx *ctx)
{
    return ctx_stop(ctx);
}

PUBLIC_API
hound_err hound_next(struct hound_ctx *ctx, size_t n)
{
    return ctx_next(ctx, n);
}

PUBLIC_API
hound_err hound_read(struct hound_ctx *ctx, size_t records)
{
    return ctx_read(ctx, records);
}

PUBLIC_API
hound_err hound_read_nowait(
    struct hound_ctx *ctx,
    size_t records,
    size_t *read)
{
    return ctx_read_nowait(ctx, records, read);
}

PUBLIC_API
hound_err hound_read_bytes_nowait(
    struct hound_ctx *ctx,
    size_t bytes,
    size_t *records_read,
    size_t *bytes_read)
{
    return ctx_read_bytes_nowait(ctx, bytes, records_read, bytes_read);
}

PUBLIC_API
hound_err hound_read_all_nowait(struct hound_ctx *ctx, size_t *read)
{
    return ctx_read_all_nowait(ctx, read);
}

PUBLIC_API
hound_err hound_queue_length(struct hound_ctx *ctx, size_t *count)
{
    return ctx_queue_length(ctx, count);
}

PUBLIC_API
hound_err hound_max_queue_length(struct hound_ctx *ctx, size_t *count)
{
    return ctx_max_queue_length(ctx, count);
}

PUBLIC_API
hound_err hound_unregister_driver(const char *path)
{
    return driver_unregister(path);
}

PUBLIC_API
const char *hound_strerror(hound_err err)
{
    return error_strerror(err);
}
