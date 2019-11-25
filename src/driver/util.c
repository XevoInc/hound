#define _POSIX_C_SOURCE 200809L
#include <hound-private/driver-ops.h>
#include <hound-private/driver/util.h>
#include <hound-private/error.h>
#include <hound-private/util.h>
#include <string.h>

PUBLIC_API
void *drv_alloc(size_t bytes)
{
    return malloc(bytes);
}

PUBLIC_API
void *drv_realloc(void *p, size_t bytes)
{
    return realloc(p, bytes);
}

PUBLIC_API
char *drv_strdup(const char *s)
{
    return strdup(s);
}

PUBLIC_API
void drv_free(void *p)
{
    free(p);
}

PUBLIC_API
void *drv_ctx(void)
{
    const struct driver *drv;

    /*
     * This should be called only from a driver's callback, so we should already
     * hold the driver's mutex.
     */
    drv = get_active_drv();
    XASSERT_NOT_NULL(drv);
    return drv->ctx;
}

PUBLIC_API
void drv_set_ctx(void *ctx)
{
    struct driver *drv;

    /*
     * This should be called only from a driver's callback, so we should already
     * hold the driver's mutex.
     */
    drv = get_active_drv();
    XASSERT_NOT_NULL(drv);
    drv->ctx = ctx;
}

PUBLIC_API
hound_err drv_deepcopy_desc(
    struct hound_datadesc *dest,
    const struct hound_datadesc *src)
{
    XASSERT_NOT_NULL(src);
    XASSERT_NOT_NULL(dest);

    dest->data_id = src->data_id;

    dest->period_count = src->period_count;
    dest->avail_periods = drv_alloc(
        src->period_count*sizeof(*src->avail_periods));
    if (dest->avail_periods == NULL) {
        return HOUND_OOM;
    }
    memcpy(
        (void *) dest->avail_periods,
        src->avail_periods,
        src->period_count*sizeof(*src->avail_periods));

    /*
     * Note that we don't copy the format information, as it is managed by the
     * driver core, and not the drivers themselves.
     */

    return HOUND_OK;
}

PUBLIC_API
void drv_destroy_desc(struct hound_datadesc *desc)
{
    size_t i;
    struct hound_data_fmt *fmt;

    drv_free((void *) desc->name);
    drv_free((void *) desc->avail_periods);

    for (i = 0; i < desc->fmt_count; ++i) {
        fmt = &desc->fmts[i];
        drv_free((void *) fmt->name);
    }
    drv_free(desc->fmts);
}
