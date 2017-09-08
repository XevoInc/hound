/**
 * @file      hound.c
 * @brief     Hound public implementation, gluing together all subsystems.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound_private/api.h>
#include <hound_private/ctx.h>
#include <hound_private/error.h>
#include <hound_private/driver.h>
#include <hound_private/log.h>

PUBLIC_API
hound_err hound_get_datadesc(const struct hound_datadesc ***desc, size_t *len)
{
    return driver_get_datadesc(desc, len);
}

PUBLIC_API
void hound_free_datadesc(const struct hound_datadesc **desc)
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
hound_err hound_read(struct hound_ctx *ctx, size_t n)
{
    return ctx_read(ctx, n);
}

PUBLIC_API
hound_err hound_read_async(struct hound_ctx *ctx, size_t n, size_t *read)
{
    return ctx_read_async(ctx, n, read);
}

PUBLIC_API
hound_err hound_read_all(struct hound_ctx *ctx, size_t *read)
{
    return ctx_read_all(ctx, read);
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
