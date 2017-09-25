/**
 * @file      ctx.h
 * @brief     Context tracking header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_CTX_H_
#define HOUND_PRIVATE_CTX_H_

#include <hound/hound.h>
#include <stdbool.h>
#include <xlib/xhash.h>

void ctx_init(void);
void ctx_destroy(void);

hound_err ctx_alloc(struct hound_ctx **ctx, const struct hound_rq *rq);
hound_err ctx_free(struct hound_ctx *ctx);

hound_err ctx_start(struct hound_ctx *ctx);
hound_err ctx_stop(struct hound_ctx *ctx);

hound_err ctx_next(struct hound_ctx *ctx, size_t n);

hound_err ctx_read_bytes(struct hound_ctx *ctx, size_t bytes);
hound_err ctx_read_bytes_async(
    struct hound_ctx *ctx,
    size_t bytes,
    size_t *records_read,
    size_t *bytes_read);
hound_err ctx_read(struct hound_ctx *ctx, size_t records);
hound_err ctx_read_async(struct hound_ctx *ctx, size_t records, size_t *read);
hound_err ctx_read_all(struct hound_ctx *ctx, size_t *read);

hound_err ctx_queue_length(struct hound_ctx *ctx, size_t *count);
hound_err ctx_max_queue_length(struct hound_ctx *ctx, size_t *count);

#endif /* HOUND_PRIVATE_CTX_H_ */
