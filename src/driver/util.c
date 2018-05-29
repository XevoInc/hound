#define _POSIX_C_SOURCE 200809L
#include <hound_private/api.h>
#include <hound_private/driver/util.h>
#include <hound_private/error.h>
#include <string.h>

PUBLIC_API
hound_err drv_deepcopy_desc(
    struct hound_datadesc *dest,
    const struct hound_datadesc *src)
{
    size_t len;

    XASSERT_NOT_NULL(src);
    XASSERT_NOT_NULL(dest);

    dest->data_id = src->data_id;

    len = strnlen(src->name, HOUND_DATA_NAME_MAX);
    XASSERT_LTE(len, HOUND_DATA_NAME_MAX-1);
    dest->name = drv_alloc((len+1)*sizeof(*dest->name));
    if (dest->name == NULL) {
        goto error_name;
    }
    memcpy((void *) dest->name, src->name, (len+1)*sizeof(*dest->name));

    dest->period_count = src->period_count;
    dest->avail_periods = drv_alloc(
        src->period_count*sizeof(*src->avail_periods));
    if (dest->avail_periods == NULL) {
        goto error_avail_periods;
    }
    memcpy(
        (void *) dest->avail_periods,
        src->avail_periods,
        src->period_count*sizeof(*src->avail_periods));

    return HOUND_OK;

error_avail_periods:
    drv_free((void *) dest->name);
error_name:
    return HOUND_OOM;
}

PUBLIC_API
void drv_destroy_desc(struct hound_datadesc *desc)
{
    drv_free((void *) desc->name);
    drv_free((void *) desc->avail_periods);
}
