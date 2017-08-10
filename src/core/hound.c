/**
 * @file      hound.c
 * @brief     Hound public implementation, gluing together all subsystems.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/hound.h>
#include <hound_private/ctx.h>
#include <hound_private/error.h>
#include <hound_private/driver.h>
#include <hound_private/log.h>

#define HOUND_PUBLIC_API __attribute__ ((visibility ("default")))

HOUND_PUBLIC_API
hound_err hound_get_datadesc(const struct hound_datadesc ***desc, size_t *len)
{
    return driver_get_datadesc(desc, len);
}

HOUND_PUBLIC_API
void hound_free_datadesc(const struct hound_datadesc **desc)
{
    driver_free_datadesc(desc);
}

HOUND_PUBLIC_API
hound_err hound_alloc_ctx(struct hound_ctx **ctx, const struct hound_rq *rq)
{
    return ctx_alloc(ctx, rq);
}

HOUND_PUBLIC_API
hound_err hound_free_ctx(struct hound_ctx *ctx)
{
    return ctx_free(ctx);
}

HOUND_PUBLIC_API
hound_err hound_start(struct hound_ctx *ctx)
{
    return ctx_start(ctx);
}

HOUND_PUBLIC_API
hound_err hound_stop(struct hound_ctx *ctx)
{
    return ctx_stop(ctx);
}

HOUND_PUBLIC_API
hound_err hound_next(struct hound_ctx *ctx, size_t n)
{
    return ctx_next(ctx, n);
}

HOUND_PUBLIC_API
hound_err hound_read(struct hound_ctx *ctx, size_t n)
{
    return ctx_read(ctx, n);
}

HOUND_PUBLIC_API
hound_err hound_read_async(struct hound_ctx *ctx, size_t n, size_t *read)
{
    return ctx_read_async(ctx, n, read);
}

HOUND_PUBLIC_API
hound_err hound_read_all(struct hound_ctx *ctx, size_t *read)
{
    return ctx_read_all(ctx, read);
}

HOUND_PUBLIC_API
hound_err hound_queue_length(struct hound_ctx *ctx, size_t *count)
{
    return ctx_queue_length(ctx, count);
}

HOUND_PUBLIC_API
hound_err hound_max_queue_length(struct hound_ctx *ctx, size_t *count)
{
    return ctx_max_queue_length(ctx, count);
}

HOUND_PUBLIC_API
hound_err hound_register_io_driver(
    const char *path,
    const struct hound_io_driver *driver,
    void *data)
{
    return driver_register(path, driver, data);
}

HOUND_PUBLIC_API
hound_err hound_unregister_io_driver(const char *path)
{
    return driver_unregister(path);
}

HOUND_PUBLIC_API
void hound_log_msg(int priority, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    log_vmsg(priority, fmt, args);
    va_end(args);
}

HOUND_PUBLIC_API
void hound_log_vmsg(int priority, const char *fmt, va_list args)
{
    log_vmsg(priority, fmt, args);
}

HOUND_PUBLIC_API
void _hound_error_log_msg(
    const char *expr,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    ...)
{
    va_list args;

    va_start(args, fmt);
    _error_log_msg(expr, file, line, func, fmt, args);
    va_end(args);
}

HOUND_PUBLIC_API
const char *hound_strerror(hound_err err)
{
    return error_strerror(err);
}
