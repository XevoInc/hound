#define _POSIX_C_SOURCE 200809L
#include <hound-private/driver-ops.h>
#include <hound-private/error.h>
#include <hound-private/parse/schema.h>
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
    XASSERT_NOT_NULL(s);

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
int drv_fd(void)
{
    struct driver *drv;

    /*
     * This should be called only from a driver's callback, so we should already
     * hold the driver's mutex.
     */
    drv = get_active_drv();
    XASSERT_NOT_NULL(drv);
    return drv->fd;
}
