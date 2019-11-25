/**
 * @file      iio.c
 * @brief     Industrial I/O driver implementation.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <endian.h>
#include <errno.h>
#include <hound/hound.h>
#include <hound/driver/iio.h>
#include <hound-private/driver.h>
#include <hound-private/driver/util.h>
#include <hound-private/error.h>
#include <hound-private/util.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define IIO_TOPDIR "/sys/bus/iio/devices"
#define FD_INVALID (-1)

struct chan_desc {
    hound_data_id id;
    const char *scale_file;
    const char *type_file;
    const char *index_file;
    const char *enable_file;
};

struct device_entry {
    hound_data_id id;
    size_t num_channels;
    const struct chan_desc *channels;
    const char *freqs_file;
    const char *freqs_avail_file;
    const char *schema;
};

struct device_parse_entry {
    hound_data_id id;
    size_t num_channels;
    size_t data_size;
    struct chan_parse_desc *channels;
};

/*
 * TODO: Instead of hardcoding, dynamically populate this by listing
 * directories, like iio_generic_buffer does.
 */

static const struct chan_desc s_accel_chan[] = {
    {
        .id = HOUND_DATA_ACCEL,
        .scale_file = "in_accel_scale",
        .type_file = "in_accel_x_type",
        .index_file = "in_accel_x_index",
        .enable_file = "in_accel_x_en"
    },
    {
        .id = HOUND_DATA_ACCEL,
        .scale_file = "in_accel_scale",
        .type_file = "in_accel_y_type",
        .index_file = "in_accel_y_index",
        .enable_file = "in_accel_y_en"
    },
    {
        .id = HOUND_DATA_ACCEL,
        .scale_file = "in_accel_scale",
        .type_file = "in_accel_z_type",
        .index_file = "in_accel_z_index",
        .enable_file = "in_accel_z_en"
    }
};

static const struct chan_desc s_gyro_chan[] = {
    {
        .id = HOUND_DATA_GYRO,
        .scale_file = "in_anglvel_scale",
        .type_file = "in_anglvel_x_type",
        .index_file = "in_anglvel_x_index",
        .enable_file = "in_anglvel_x_en"
    },
    {
        .id = HOUND_DATA_GYRO,
        .scale_file = "in_anglvel_scale",
        .type_file = "in_anglvel_y_type",
        .index_file = "in_anglvel_y_index",
        .enable_file = "in_anglvel_y_en"
    },
    {
        .id = HOUND_DATA_GYRO,
        .scale_file = "in_anglvel_scale",
        .type_file = "in_anglvel_z_type",
        .index_file = "in_anglvel_z_index",
        .enable_file = "in_anglvel_z_en"
    }
};

/** Static map from data type to channel descriptors. */
static const struct device_entry s_channels[] = {
    {
        .id = HOUND_DATA_ACCEL,
        .num_channels = 3,
        .channels = s_accel_chan,
        .freqs_file = "in_accel_sampling_frequency",
        .freqs_avail_file = "in_accel_sampling_frequency_available",
        .schema = "accel.yaml"
    },
    {
        .id = HOUND_DATA_GYRO,
        .num_channels = 3,
        .channels = s_gyro_chan,
        .freqs_file = "in_anglvel_sampling_frequency",
        .freqs_avail_file = "in_anglvel_sampling_frequency_available",
        .schema = "gyro.yaml"
    }
};

#define DESC_COUNT_MAX ARRAYLEN(s_channels)

static struct chan_desc s_timestamp_chan = {
    /* Does not really have an ID, so set it to a bogus value. */
    .id = UINT32_MAX,
    .scale_file = NULL,
    .type_file = "in_timestamp_type",
    .index_file = "in_timestamp_index",
    .enable_file = "in_timestamp_en"
};

struct chan_parse_desc {
    const struct chan_desc *chan;
    /** Copies the data from raw source buffers to formatted destination
     * buffers, applying endianness, mask, and shift operators as appropriate.
     */
    /** Copies the data, respecting endianness, scale, mask, and shift. */
    void (*copy_func)(
        uint8_t *dest,
        const uint8_t *src,
        uint_fast8_t shift,
        uint_fast64_t mask);
    /** Uses the copy_func to copy the data, then casts to float. */
    float (*copy_func_float)(
        const uint8_t *src,
        uint_fast8_t shift,
        uint_fast64_t mask);
    /** Scale factor to apply to raw values. */
    float scale;
    /** The amount to right-shift the incoming data. */
    uint_fast8_t shift;
    /** The bitmask to apply to the incoming data. */
    uint_fast64_t mask;
    /** The number of meaningful bytes. */
    uint_fast8_t data_bytes;
    /** The number of storage bytes. */
    uint_fast8_t storage_bytes;
    /** The index in the scan at which this channel is found. */
    size_t index;
};

struct iio_ctx {
    bool active;
    char *dev;
    char *dev_dir;
    char dev_name[HOUND_DATA_NAME_MAX];
    uint_fast64_t buf_ns;
    size_t num_entries;
    struct device_parse_entry *entries;
    struct chan_parse_desc timestamp_channel;
    size_t scan_size;
};

struct chan_sort_entry {
    int index;
    struct chan_desc *chan;
};

#define ENDIAN_FUNC(bits, endian) endian##bits##toh
#define identity_copy(x) (x)
#define BITS_TYPE_UNSIGNED(bits) uint##bits##_t
#define BITS_TYPE_UNSIGNED_FAST(bits) uint##bits##_t
#define BITS_TYPE_SIGNED(bits) int##bits##_t
#define BITS_TYPE_SIGNED_FAST(bits) int##bits##_t

#define _DEFINE_COPY_FUNC(bits, name, endian, endian_func, type) \
static inline \
void endian##bits##_copy_##name( \
    uint8_t *dest, \
    const uint8_t *src, \
    uint_fast8_t shift, \
    uint_fast64_t mask) \
{ \
    BITS_TYPE_UNSIGNED_FAST(bits) u; \
    \
    /*
     * Treat as unsigned until we finally cast to avoid sign extension in the
     * shift.
     */ \
    u = endian_func(*(BITS_TYPE_UNSIGNED(bits) *) src); \
    u >>= shift; \
    u &= mask; \
    \
    *((type *) dest) = u; \
}

#define _DEFINE_COPY_FUNC_FLOAT(bits, name, endian, endian_func, fast_type) \
static inline \
float endian##bits##_copy_##name##_float( \
    const uint8_t *src, \
    uint_fast8_t shift, \
    uint_fast64_t mask) \
{ \
    fast_type t; \
    \
    endian##bits##_copy_##name((uint8_t *) &t, src, shift, mask); \
    \
    return (float) t; \
}

#define _DEFINE_COPY_FUNC_UNSIGNED(bits, endian, endian_func) \
    _DEFINE_COPY_FUNC(\
        bits, \
        unsigned, \
        endian, \
        endian_func, \
        BITS_TYPE_UNSIGNED(bits)) \
    _DEFINE_COPY_FUNC_FLOAT(\
        bits, \
        unsigned, \
        endian, \
        endian_func, \
        BITS_TYPE_UNSIGNED_FAST(bits))

#define _DEFINE_COPY_FUNC_SIGNED(bits, endian, endian_func) \
    _DEFINE_COPY_FUNC(\
        bits, \
        signed, \
        endian, \
        endian_func, \
        BITS_TYPE_SIGNED(bits)) \
    _DEFINE_COPY_FUNC_FLOAT(\
        bits, \
        signed, \
        endian, \
        ENDIAN_FUNC(bits, endian), \
        BITS_TYPE_SIGNED_FAST(bits))

#define DEFINE_COPY_FUNC_ENDIAN(bits, endian, endian_func) \
    _DEFINE_COPY_FUNC_UNSIGNED(bits, endian, endian_func) \
    _DEFINE_COPY_FUNC_SIGNED(bits, endian, endian_func)

#define DEFINE_COPY_FUNC(bits) \
    DEFINE_COPY_FUNC_ENDIAN(bits, be, ENDIAN_FUNC(bits, be)) \
    DEFINE_COPY_FUNC_ENDIAN(bits, le, ENDIAN_FUNC(bits, le)) \

/* Special-case one-byte copying, as the endian function is a no-op. */
DEFINE_COPY_FUNC_ENDIAN(8, identity, identity_copy)
DEFINE_COPY_FUNC(16)
DEFINE_COPY_FUNC(32)
DEFINE_COPY_FUNC(64)

static
void iio_make_path(const char *dev_dir, char *path, size_t maxlen, const char *file)
{
    hound_err err;

    err = snprintf(path, maxlen, "%s/%s", dev_dir, file);
    XASSERT_GT(err, 0);
}

static inline
hound_err iio_get_freq(hound_data_period period, hound_data_period *freq)
{
    hound_data_period hz;

    hz = NSEC_PER_SEC / period;
    /*
     * Currently we handle don't handle fractional frequencies, though IIO
     * supports them. If we need to handle them, we need to convert to an
     * exact fraction (with no extra zeroes), since the frequency is written
     * into sysfs as a string.
     */
    if (hz*period != NSEC_PER_SEC) {
        return HOUND_DRIVER_UNSUPPORTED;
    }

    *freq = hz;

    return HOUND_OK;
}

static inline
hound_err iio_get_period(hound_data_period hz, hound_data_period *out)
{
    hound_data_period period;

    period = NSEC_PER_SEC / hz;
    /*
     * Currently we handle don't handle fractional frequencies, though IIO
     * supports them. If we need to handle them, we need to convert to an
     * exact fraction (with no extra zeroes), since the frequency is written
     * into sysfs as a string.
     */
    if (hz*period != NSEC_PER_SEC) {
        return HOUND_DRIVER_UNSUPPORTED;
    }

    *out = period;

    return HOUND_OK;
}

static
hound_err iio_read_abs(
    const char *path,
    char *out,
    size_t maxlen,
    size_t *bytes_read)
{
    ssize_t bytes;
    const char *end;
    hound_err err;
    int fd;
    char *pos;
    char *tmp;

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        return errno;
    }

    err = HOUND_OK;
    pos = out;
    end = out + maxlen;
    do {
        bytes = read(fd, pos, end - pos);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            else if (errno == EIO || errno == EBUSY) {
                err = errno;
                goto out;
            }
            else {
                XASSERT_ERROR;
            }
        }
        pos += bytes;
    } while (bytes == (end-pos) && pos < end);
    *(pos-1) = '\0';

    /* Swallow the new-line if there is one. */
    for (tmp = pos-2; tmp > out; --tmp) {
        if (*tmp != '\0') {
            if (*tmp == '\n') {
                *tmp = '\0';
            }
            break;
        }
    }

out:
    close(fd);
    if (bytes_read != NULL) {
        *bytes_read = pos - out;
    }
    return err;
}

static
hound_err iio_read(
    const char *dev_dir,
    const char *file,
    char *out,
    size_t maxlen,
    size_t *bytes_read)
{
    char path[PATH_MAX];

    iio_make_path(dev_dir, path, ARRAYLEN(path), file);

    return iio_read_abs(path, out, maxlen, bytes_read);
}

static
hound_err iio_write(
    const char *dev_dir,
    const char *file,
    const char *s,
    size_t len)
{
    ssize_t bytes;
    hound_err err;
    int fd;
    char path[PATH_MAX];
    size_t total;

    err = snprintf(path, ARRAYLEN(path), "%s/%s", dev_dir, file);
    XASSERT_GT(err, 0);

    fd = open(path, O_WRONLY);
    if (fd == -1) {
        return errno;
    }

    err = HOUND_OK;
    total = 0;
    do {
        bytes = write(fd, s+total, len-total);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            else if (errno == EIO || errno == EBUSY) {
                err = errno;
                break;
            }
            else {
                XASSERT_ERROR;
            }
        }
        total += bytes;
    } while (total < len);
    close(fd);

    return err;
}

static
hound_err parse_avail_periods(
    const char *dev_dir,
    const char *freqs_avail_file,
    hound_period_count *count,
    hound_data_period **avail_periods)
{
    char buf[PATH_MAX];
    size_t bytes;
    hound_err err;
    FILE *f;
    hound_data_period freq;
    size_t i;
    size_t n;
    hound_data_period *p;

    XASSERT_NOT_NULL(count);
    XASSERT_NOT_NULL(avail_periods);

    iio_make_path(
        dev_dir,
        buf,
        ARRAYLEN(buf),
        freqs_avail_file);
    f = fopen(buf, "r");
    if (f == NULL) {
        /*
         * fopen/fread don't set errno, so sadly we can't retrieve the real
         * error here. Using read/write for this is substantially more
         * complicated because of the lack of fscanf, so we have to handle
         * buffering and partial reads (reading half a number). Alternatively,
         * we could assume our buffer contains the entire file, but that seems
         * brittle. Given the choices, just fscanf and returning a generic
         * HOUND_IO_ERROR seems like the best of the bad options.
         */
        return HOUND_IO_ERROR;
    }

    /* Count number of items so we can allocate the right amount. */
    n = 1;
    while (true) {
        bytes = fread(buf, 1, ARRAYLEN(buf), f);
        if (ferror(f)) {
            err = HOUND_IO_ERROR;
            goto out;
        }

        for (i = 0; i < bytes; ++i) {
            if (buf[i] == ' ') {
                ++n;
            }
        }

        if (bytes < ARRAYLEN(buf)) {
            break;
        }
    }

    p = malloc(n * sizeof(**avail_periods));
    if (p == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    rewind(f);
    for (i = 0; i < n; ++i) {
        err = fscanf(f, "%" PRIu64, &freq);
        if (err != 1) {
            err = errno;
            goto error_fscanf;
        }
        err = iio_get_period(freq, &p[i]);
        if (err != HOUND_OK) {
            goto error_period;
        }
    }

    *count = n;
    *avail_periods = p;
    err = HOUND_OK;
    goto out;

error_period:
error_fscanf:
    free(p);
out:
    fclose(f);
    return err;
}

static
hound_err iio_disable_device(const char *dev_dir)
{
    return iio_write(dev_dir, "buffer/enable", "0", 1);
}

static
hound_err iio_enable_device(const char *dev_dir)
{
    return iio_write(dev_dir, "buffer/enable", "1", 1);
}

static
hound_err iio_start(int *out_fd)
{
    struct iio_ctx *ctx;
    hound_err err;
    int fd;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);
    XASSERT_NOT_NULL(ctx->dev_dir);

    fd = open(ctx->dev, 0);
    if (fd == -1) {
        err = errno;
        goto out;
    }

    err = iio_enable_device(ctx->dev_dir);
    if (err != HOUND_OK) {
        goto error;
    }

    ctx->active = true;
    *out_fd = fd;
    goto out;

error:
    close(fd);
out:
    return err;
}

static
hound_err iio_stop(void)
{
    struct iio_ctx *ctx;
    hound_err err;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);
    XASSERT_NOT_NULL(ctx->dev_dir);

    err = iio_disable_device(ctx->dev_dir);
    if (err == HOUND_OK) {
        ctx->active = false;
    }

    return err;
}

static
hound_err iio_set_clock(const char *dev_dir, const char *clock_type, size_t len)
{
    return iio_write(dev_dir, "current_timestamp_clock", clock_type, len);
}

static
hound_err iio_init(void *data)
{
    char buf[2];
    struct iio_ctx *ctx;
    char *dev_dir;
    hound_err err;
    const struct hound_iio_driver_init *init;
    char path[PATH_MAX];
    struct stat st;

    if (data == NULL) {
        return HOUND_NULL_VAL;
    }
    init = data;

    /* Validate parameters. */
    if (init->dev == NULL) {
        return HOUND_NULL_VAL;
    }

    if (init->buf_ns == 0) {
        return HOUND_INVALID_VAL;
    }

    /* Verify the device exists and is usable. */
    err = access(init->dev, R_OK);
    if (err != 0) {
        err = errno;
    }

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    dev_dir = basename(init->dev);
    /* We were able to access the device, so it should have a basename. */
    XASSERT_NOT_NULL(dev_dir);
    err = snprintf(path, ARRAYLEN(path), "%s/%s", IIO_TOPDIR, dev_dir);
    XASSERT_GT(err, 0);

    ctx->dev_dir = drv_strdup(path);
    if (ctx->dev_dir == NULL) {
        goto error_dev_dir;
    }

    err = stat(ctx->dev_dir, &st);
    if (err != 0) {
        goto error_stat;
    }
    XASSERT(S_ISDIR(st.st_mode));

    ctx->dev = strndup(init->dev, PATH_MAX);
    if (ctx->dev_dir == NULL) {
        err = HOUND_OOM;
        goto error_dev;
    }

    err = iio_read(
        ctx->dev_dir,
        "name",
        ctx->dev_name,
        ARRAYLEN(ctx->dev_name),
        NULL);
    if (err != HOUND_OK) {
        goto error_dev_name;
    }

    err = iio_read(ctx->dev_dir, "buffer/enable", buf, ARRAYLEN(buf), NULL);
    if (err != HOUND_OK) {
        goto error_buffer_enable;
    }
    if (*buf == '0') {
        ctx->active = false;
    }
    else if (*buf == '1') {
        ctx->active = true;
    }
    else {
        XASSERT_ERROR;
    }
    ctx->buf_ns = init->buf_ns;
    ctx->num_entries = 0;
    ctx->scan_size = 0;
    ctx->entries = NULL;
    drv_set_ctx(ctx);
    err = HOUND_OK;
    goto out;

error_buffer_enable:
error_dev_name:
error_dev:
error_stat:
    free(ctx->dev_dir);
error_dev_dir:
    free(ctx);
out:
    return err;
}

static
void free_parse_entries(struct device_parse_entry *entries, size_t num_entries)
{
    size_t i;

    for (i = 0; i < num_entries; ++i) {
        free(entries[i].channels);
    }
    free(entries);
}

static
hound_err iio_destroy(void)
{
    struct iio_ctx *ctx;

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    free(ctx->dev);
    free(ctx->dev_dir);
    free_parse_entries(ctx->entries, ctx->num_entries);
    free(ctx);

    return HOUND_OK;
}

static
hound_err iio_device_name(char *device_name)
{
    const struct iio_ctx *ctx;

    XASSERT_NOT_NULL(device_name);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);
    XASSERT_NOT_NULL(ctx->dev_dir);

    strncpy(device_name, ctx->dev_name, HOUND_DEVICE_NAME_MAX);

    return HOUND_OK;
}

static
bool iio_scan_readable(
    const char *dev_dir,
    const char *file)
{
    hound_err err;
    char path[PATH_MAX];

    err = snprintf(path, ARRAYLEN(path), "%s/scan_elements/%s", dev_dir, file);
    XASSERT_GT(err, 0);

    return (access(path, R_OK) == 0);
}

static
hound_err iio_datadesc(
    struct hound_datadesc **out,
    const char ***schemas,
    hound_data_count *count)
{
    hound_data_period *avail_periods;
    const struct chan_desc *channels;
    struct iio_ctx *ctx;
    struct hound_datadesc *desc;
    size_t desc_count;
    const struct device_entry *entry;
    hound_err err;
    size_t i;
    size_t j;
    hound_period_count period_count;

    XASSERT_NOT_NULL(out);
    XASSERT_NOT_NULL(schemas);
    XASSERT_NOT_NULL(count);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    /*
     * Preallocate more data than we probably need. We will realloc it once
     * we've scanned the system and know the correct size.
     */
    desc = malloc(DESC_COUNT_MAX * sizeof(*desc));
    if (desc == NULL) {
        err = HOUND_OOM;
        goto out;
    }

    *schemas = malloc(DESC_COUNT_MAX * sizeof(**schemas));
    if (*schemas == NULL) {
        err = HOUND_OOM;
        goto error_desc;
    }

    /*
     * This loop is not very efficient, but we do it only once. The alternative
     * is first building a hash-map, which is probably slower since we're doing
     * the lookup exactly once.
     */
    desc_count = 0;
    for (i = 0; i < ARRAYLEN(s_channels); ++i) {
        entry = &s_channels[i];
        channels = entry->channels;
        for (j = 0; j < entry->num_channels; ++j) {
            if (!iio_scan_readable(ctx->dev_dir, channels[j].type_file) ||
                !iio_scan_readable(ctx->dev_dir, channels[j].index_file) ||
                !iio_scan_readable(ctx->dev_dir, channels[j].enable_file)) {
                break;
            }
        }
        /*
         * We must have not found one of the channel names we were looking
         * for, so this device doesn't support that data type. Keep looking
         * for the next data type.
         */
        if (j != entry->num_channels) {
            continue;
        }

        desc[desc_count].data_id = entry->id;
        (*schemas)[desc_count] = entry->schema;
        ++desc_count;
    }

    if (desc_count == 0) {
        /* We have a device but no channels! */
        err = HOUND_DRIVER_FAIL;
        goto error_desc;
    };

    desc = realloc(desc, desc_count*sizeof(*desc));
    if (desc == NULL) {
        err = HOUND_OOM;
        goto error_desc;
    };

    *schemas = realloc(*schemas, desc_count*sizeof(**schemas));
    if (*schemas == NULL) {
        err = HOUND_OOM;
        goto error_desc;
    };

    /*
     * The periods are the same for all channels, so just parse them once and
     * set them for each descriptor.
     */
    err = parse_avail_periods(
        ctx->dev_dir,
        entry->freqs_avail_file,
        &period_count,
        &avail_periods);
    if (err != HOUND_OK) {
        goto error_parse_avail_periods;
    }

    desc[0].period_count = period_count;
    desc[0].avail_periods = avail_periods;
    for (i = 1; i < desc_count; ++i) {
        desc[i].period_count = period_count;
        desc[i].avail_periods = malloc(period_count * sizeof(*avail_periods));
        if (desc[i].avail_periods == NULL) {
            goto error_alloc_avail_periods;
        }

        memcpy(
            (hound_data_period *) desc[i].avail_periods,
            avail_periods,
            period_count * sizeof(*avail_periods));
    }

    *out = desc;
    *count = desc_count;
    err = HOUND_OK;
    goto out;

error_alloc_avail_periods:
    for (--i; i < desc_count; --i) {
        free((hound_data_period *) desc[i].avail_periods);
    }
error_parse_avail_periods:
    free((void *) *schemas);
error_desc:
    free(desc);
out:
    return err;
}

static
hound_err iio_set_period(
    const char *dev_dir,
    const char *freqs_file,
    hound_data_period period)
{
    hound_err err;
    hound_data_period hz;
    /* Should be much more than needed. */
    char freq[20];
    int len;

    err = iio_get_freq(period, &hz);
    if (err != HOUND_OK) {
        return err;
    }

    len = snprintf(freq, ARRAYLEN(freq), "%" PRIu64, hz);
    XASSERT_GT(len, 0);

    return iio_write(dev_dir, freqs_file, freq, len);
}

static
hound_err iio_write_chan(
    const char *dev_dir,
    const char *enable_file,
    const char *val)
{
    hound_err err;
    char file[PATH_MAX];

    err = snprintf(file, ARRAYLEN(file), "scan_elements/%s", enable_file);
    XASSERT_GT(err, 0);

    return iio_write(dev_dir, file, val, ARRAYLEN("1"));
}

static
hound_err iio_enable_chan(const char *dev_dir, const char *enable_file)
{
    return iio_write_chan(dev_dir, enable_file, "1");
}

static
hound_err iio_disable_chan(const char *dev_dir, const char *enable_file)
{
    return iio_write_chan(dev_dir, enable_file, "0");
}

static
hound_err iio_populate_parse_desc(
    const struct chan_desc *chan,
    const char *dev_dir,
    struct chan_parse_desc *desc)
{
    /* Must fit the type and scale files. */
    char buf[50];
    char *end;
    hound_err err;
    char file[PATH_MAX];
    bool is_big_endian;
    bool is_unsigned;
    const char *p;
    size_t read_bytes;
    uint64_t val;

    if (chan->scale_file != NULL) {
        err = iio_read(dev_dir, chan->scale_file, buf, ARRAYLEN(buf), NULL);
        if (err != HOUND_OK) {
            return err;
        }
        desc->scale = strtof(buf, &end);
        XASSERT_NEQ(buf, end);
    }
    else {
        desc->scale = 1.0;
    }

    err = snprintf(file, ARRAYLEN(file), "scan_elements/%s", chan->type_file);
    XASSERT_GT(err, 0);

    err = iio_read(dev_dir, file, buf, ARRAYLEN(buf), &read_bytes);
    if (err != HOUND_OK) {
        return err;
    }
    XASSERT_GT(read_bytes, 0);
    p = buf;

    if (strncmp(p, "be", 2) == 0) {
        is_big_endian = true;
    }
    else if (strncmp(p, "le", 2) == 0) {
        is_big_endian = false;
    }
    else {
        XASSERT_ERROR;
    }
    p += 3;

    if (*p == 'u') {
        is_unsigned = true;
    }
    else if (*p == 's') {
        is_unsigned = false;
    }
    else {
        XASSERT_ERROR;
    }
    p += 1;

    val = strtoul(p, &end, 10);
    XASSERT_GT(end, p);
    /*
     * We cast to uintptr_t to keep GCC happy. With out it, it issues a spurious
     * "array subscript out of bounds" warning!
     */
    XASSERT_LT((uintptr_t) end, (uintptr_t) p + (uintptr_t) ARRAYLEN(buf));
    XASSERT_LTE(val, UINT8_MAX);
    desc->data_bytes = val / 8;
    desc->mask = (1 << val) - 1;

    XASSERT_EQ(*end, '/');
    p = end + 1;

    val = strtoul(p, &end, 10);
    XASSERT_NEQ(end, p);
    XASSERT_LT(end, p + ARRAYLEN(buf));
    XASSERT_LTE(val, 64);
    XASSERT_EQ(val % 8, 0);
    desc->storage_bytes = val / 8;
    p = end;

    switch (val) {
        case 8:
            /* Endianness doesn't apply to one byte. */
            if (is_unsigned) {
                desc->copy_func = identity8_copy_unsigned;
                desc->copy_func_float = identity8_copy_unsigned_float;
            }
            else {
                desc->copy_func = identity8_copy_signed;
                desc->copy_func_float = identity8_copy_signed_float;
            }
            break;
        case 16:
            if (is_big_endian) {
                if (is_unsigned) {
                    desc->copy_func = be16_copy_unsigned;
                    desc->copy_func_float = be16_copy_unsigned_float;
                }
                else {
                    desc->copy_func = be16_copy_signed;
                    desc->copy_func_float = be16_copy_signed_float;
                }
            }
            else {
                if (is_unsigned) {
                    desc->copy_func = le16_copy_unsigned;
                    desc->copy_func_float = le16_copy_unsigned_float;
                }
                else {
                    desc->copy_func = le16_copy_signed;
                    desc->copy_func_float = le16_copy_signed_float;
                }
            }
            break;
        case 32:
            if (is_big_endian) {
                if (is_unsigned) {
                    desc->copy_func = be32_copy_unsigned;
                    desc->copy_func_float = be32_copy_unsigned_float;
                }
                else {
                    desc->copy_func = be32_copy_signed;
                    desc->copy_func_float = be32_copy_signed_float;
                }
            }
            else {
                if (is_unsigned) {
                    desc->copy_func = le32_copy_unsigned;
                    desc->copy_func_float = le32_copy_unsigned_float;
                }
                else {
                    desc->copy_func = le32_copy_signed;
                    desc->copy_func_float = le32_copy_signed_float;
                }
            }
            break;
        case 64:
            if (is_big_endian) {
                if (is_unsigned) {
                    desc->copy_func = be64_copy_unsigned;
                    desc->copy_func_float = be64_copy_unsigned_float;
                }
                else {
                    desc->copy_func = be64_copy_signed;
                    desc->copy_func_float = be64_copy_signed_float;
                }
            }
            else {
                if (is_unsigned) {
                    desc->copy_func = le64_copy_unsigned;
                    desc->copy_func_float = le64_copy_unsigned_float;
                }
                else {
                    desc->copy_func = le64_copy_signed;
                    desc->copy_func_float = le64_copy_signed_float;
                }
            }
            break;
        default:
            /* If we encounter hardware this strange, we'll deal with it. */
            XASSERT_ERROR;
    }

    if (*p != '\0') {
        /* Skip past ">>". */
        p += 2;
        /* Specifying a shift is optional. */
        desc->shift = strtoul(p, &end, 10);
        XASSERT_NEQ(end, p);
        XASSERT_LT(end, p + ARRAYLEN(buf));
    }
    else {
        desc->shift = 0;
    }
    XASSERT_EQ(*end, '\0');

    desc->chan = chan;

    return HOUND_OK;
}

static
hound_err iio_get_index(const char *dev_dir, const char *index_file, int *index)
{
    hound_err err;
    FILE *f;
    char path[PATH_MAX];

    err = snprintf(
        path,
        ARRAYLEN(path),
        "%s/scan_elements/%s",
        dev_dir,
        index_file);
    XASSERT_GT(err, 0);

    f = fopen(path, "r");
    if (f == NULL) {
        return errno;
    }

    err = fscanf(f, "%d", index);
    if (err != 1) {
        err = errno;
    }
    else {
        err = HOUND_OK;
    }
    fclose(f);

    return err;
}

static
int chan_cmp(const void *p1, const void *p2)
{
    const struct chan_sort_entry *a;
    const struct chan_sort_entry *b;

    a = p1;
    b = p2;

    if (a->index < b->index) {
        return -1;
    }
    else if (a->index == b->index) {
        return 0;
    }
    else {
        return 1;
    }
}

static
hound_err get_channel_sort_entries(
    const char *dev_dir,
    size_t num_channels,
    const struct hound_data_rq_list *data_list,
    struct chan_sort_entry *sort_entries)
{
    const struct chan_desc *channels;
    const struct device_entry *entry;
    size_t entry_index;
    hound_err err;
    size_t i;
    int index;
    size_t j;
    size_t k;
    struct chan_sort_entry *sort_entry;

    /*
     * The channel indices do not depend on what is currently enabled, so if we
     * have not enabled all possible channels, then the max index could be
     * greater than the max index that we care about. Thus we need to collect
     * all indices and sort them to determine the order in which they will be
     * read.
     */
    entry_index = 0;
    for (i = 0; i < data_list->len; ++i) {
        for (j = 0; j < ARRAYLEN(s_channels); ++j) {
            entry = &s_channels[j];
            if (entry->id != data_list->data[i].id) {
                continue;
            }
            channels = entry->channels;

            for (k = 0; k < entry->num_channels; ++k) {
                err = iio_get_index(dev_dir, channels[k].index_file, &index);
                if (err != HOUND_OK) {
                    return err;
                }

                sort_entry = &sort_entries[entry_index];
                sort_entry->index = index;
                /* We are not actually going to change this, so the cast is safe. */
                sort_entry->chan = (struct chan_desc *) &channels[k];
                ++entry_index;
            }
        }
    }

    /* Add the timestamp channel as a special case. */
    err = iio_get_index(dev_dir, s_timestamp_chan.index_file, &index);
    if (err != HOUND_OK) {
        return err;
    }
    sort_entry = &sort_entries[entry_index];
    sort_entry->index = index;
    sort_entry->chan = &s_timestamp_chan;

    qsort(sort_entries, num_channels, sizeof(*sort_entries), chan_cmp);

    return HOUND_OK;
}

static
hound_err iio_set_buffer_length(const char *dev_dir, uint_fast64_t n)
{
    /* Must fit the max integer width possible. */
    char buf[30];
    int len;

    len = snprintf(buf, ARRAYLEN(buf), "%" PRIu64, n);
    XASSERT_GT(len, 0);

    return iio_write(dev_dir, "buffer/length", buf, len);
}

/**
 * Calculates the scan size and sets the read index for each channel in the
 * parse entry array.
 *
 * @param sort_entries channel entries, presorted by get_channel_sort_entries
 * @param num_channels the number of channel sort entries
 * @param entries a pointer to the parse entries to fill in
 * @param timestamp_desc the parse descriptor for the timestamp channel
 *
 * @return the scan size, in bytes
 */
static
size_t iio_finalize_scan(
    struct chan_sort_entry *sort_entries,
    size_t num_channels,
    struct device_parse_entry *entries,
    struct chan_parse_desc *timestamp_desc)
{
    const struct chan_desc *chan;
    struct chan_parse_desc *desc;
    struct device_parse_entry *entry;
    size_t entry_index;
    size_t i;
    size_t offset;

    offset = 0;
    entry_index = 0;
    entry = entries;
    for (i = 0; i < num_channels; ++i) {
        chan = sort_entries[i].chan;
        if (chan == &s_timestamp_chan) {
            desc = timestamp_desc;
        }
        else {
            desc = &entry->channels[entry_index];
        }

        /*
         * If the new channel evenly divides the end offset of the previous
         * channel, then the channels will be tightly packed right next to each
         * other. Otherwise, put it at the next even multiple of the new channel
         * size.
         */
        if (offset % desc->storage_bytes == 0) {
            desc->index = offset;
        }
        else {
            desc->index =
                offset - offset%desc->storage_bytes + desc->storage_bytes;
        }
        offset = desc->index + desc->storage_bytes;

        /*
         * The timestamp channel does not get a parse entry, as it's treated
         * specially.
         */
        if (desc == timestamp_desc) {
            continue;
        }

        ++entry_index;
        if (entry_index == entry->num_channels) {
            ++entry;
            entry_index = 0;
        }
    }

    return offset;
}

static
hound_err iio_setdata(const struct hound_data_rq_list *data_list)
{
    uint_fast64_t buf_samples;
    double buf_sec;
    const struct chan_desc *chan;
    size_t chan_num = 0;
    struct iio_ctx *ctx;
    const struct device_entry *entry;
    hound_err err;
    bool found_id;
    hound_data_period hz;
    size_t i;
    hound_data_id id;
    size_t j;
    size_t entry_index;
    size_t num_channels;
    struct device_parse_entry *parse_entry = NULL;
    hound_data_period period;
    bool restart;
    const struct chan_sort_entry *sort_entry;
    struct chan_sort_entry *sort_entries;

    XASSERT_NOT_NULL(data_list);
    XASSERT_GT(data_list->len, 0);
    XASSERT_NOT_NULL(data_list->data);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);
    XASSERT_NOT_NULL(ctx->dev_dir);

    /* If we're currently active, we need to stop and start the device first. */
    restart = ctx->active;
    if (restart) {
        err = iio_disable_device(ctx->dev_dir);
        if (err != HOUND_OK) {
            return err;
        }

        /* Disable any currently active channels. */
        for (i = 0; i < ctx->num_entries; ++i) {
            parse_entry = &ctx->entries[i];
            for (j = 0; j < parse_entry->num_channels; ++j) {
                chan = parse_entry->channels[j].chan;
                err = iio_disable_chan(ctx->dev_dir, chan->enable_file);
                if (err != HOUND_OK) {
                    goto out_error;
                }
            }
        }
    }

    /* For simplicity, we use only the realtime clock. */
    err = iio_set_clock(ctx->dev_dir, "realtime", ARRAYLEN("realtime"));
    if (err != HOUND_OK) {
        goto out_error;
   }

    /* Set the data frequency, and calculate the buffer we'll need. */
    buf_sec = ((double) ctx->buf_ns) / NSEC_PER_SEC;
    buf_samples = 0;
    for (i = 0; i < data_list->len; ++i) {
        period = data_list->data[i].period_ns;
        /* Find our corresponding device entry. */
        for (j = 0; j < ARRAYLEN(s_channels); ++j) {
            entry = &s_channels[j];
            if (entry->id != data_list->data[i].id) {
                continue;
            }
            err = iio_set_period(
                ctx->dev_dir, 
                entry->freqs_file,
                period);
            if (err != HOUND_OK) {
                goto out_error;
            }

            err = iio_get_freq(period, &hz);
            if (err != HOUND_OK) {
                goto out_error;
            }
            buf_samples += (uint_fast64_t) (hz*buf_sec);
        }
    }

    /* Set the buffer length to buffer the amount of time the user requested. */
    err = iio_set_buffer_length(ctx->dev_dir, buf_samples);
    if (err != HOUND_OK) {
        goto out_error;
    }

    /*
     * We always set the watermark to 1 so that poll will return immediately
     * when a sample is available.
     */
    err = iio_write(ctx->dev_dir, "buffer/watermark", "1", 1);
    if (err != HOUND_OK) {
        goto out_error;
    }

    /*
     * Preallocate all the channels we need, including the special timestamp channel.
     */
    num_channels = 1;
    for (i = 0; i < data_list->len; ++i) {
        for (j = 0; j < ARRAYLEN(s_channels); ++j) {
            entry = &s_channels[j];
            if (entry->id != data_list->data[i].id) {
                continue;
            }
            num_channels += entry->num_channels;
        }
    }

    sort_entries = malloc(num_channels*sizeof(*sort_entries));
    if (sort_entries == NULL) {
        err = HOUND_OOM;
        goto out_error;
    }

    err = get_channel_sort_entries(ctx->dev_dir, num_channels, data_list, sort_entries);
    if (err != HOUND_OK) {
        err = HOUND_OOM;
        goto out_error;
    }

    /*
     * Create the device parse entries. Note that timestamp doesn't get an
     * entry, as it is special-cased, since we produce one timestamp for each
     * record. Further note that, since the driver core protects us against a
     * request specifying the same ID multiple times, the size of the data list
     * is also the number of unique data IDs we are handling.
     */
    ctx->num_entries = data_list->len;
    if (ctx->entries != NULL) {
        free_parse_entries(ctx->entries, ctx->num_entries);
    }
    ctx->entries = malloc(ctx->num_entries * sizeof(*ctx->entries));
    if (ctx->entries == NULL) {
        err = HOUND_OOM;
        goto error_malloc_entries;
    }

    /* Enable the requested channels. */
    found_id = false;
    entry_index = 0;
    for (i = 0; i < num_channels-1; ++i) {
        sort_entry = &sort_entries[i];
        chan = sort_entry->chan;

        /*
         * This channel maps to a new parse entry; populate the entry. Note that
         * timestamp doesn't get an entry, as it's parsed differently than the
         * other channels.
         */
        if (!found_id || chan->id != id) {
            id = chan->id;
            found_id = true;
            chan_num = 0;
            parse_entry = &ctx->entries[entry_index];
            for (j = i+1; j < num_channels; ++j) {
                if (sort_entries[j].chan->id != id) {
                    break;
                }
            }
            parse_entry->id = id;
            parse_entry->num_channels = j - i;
            parse_entry->data_size =
                parse_entry->num_channels * sizeof(float);

            parse_entry->channels =
                malloc(parse_entry->num_channels * sizeof(*parse_entry->channels));
            if (parse_entry->channels == NULL) {
                goto error_chan_parse;
            }

            ++entry_index;
        }
        else {
            ++chan_num;
        }

        err = iio_populate_parse_desc(
            chan,
            ctx->dev_dir,
            &parse_entry->channels[chan_num]);
        if (err != HOUND_OK) {
            goto error_chan_parse;
        }

        err = iio_enable_chan(ctx->dev_dir, chan->enable_file);
        if (err != HOUND_OK) {
            goto error_chan_parse;
        }
    }

    /*
     * Special-case the timestamp channel, since it's handled differently in
     * parsing.
     */
    err = iio_populate_parse_desc(
        &s_timestamp_chan,
        ctx->dev_dir,
        &ctx->timestamp_channel);
    if (err != HOUND_OK) {
        goto error_chan_parse;
    }

    err = iio_enable_chan(ctx->dev_dir, s_timestamp_chan.enable_file);
    if (err != HOUND_OK) {
        goto error_chan_parse;
    }

    ctx->scan_size = iio_finalize_scan(
        sort_entries,
        num_channels,
        ctx->entries,
        &ctx->timestamp_channel);

    err = HOUND_OK;
    goto out_success;

error_chan_parse:
    for (; i < num_channels-1; ++i) {
        free(ctx->entries[i].channels);
    }
    free(ctx->entries);
error_malloc_entries:
out_success:
    free(sort_entries);
out_error:
    if (restart) {
        err = iio_enable_device(ctx->dev_dir);
        if (err != HOUND_OK) {
            hound_log_err_nofmt(err, "failed to enable device");
        }
    }

    return err;
}

static
hound_err iio_make_record(
    const struct device_parse_entry *entry,
    const uint8_t *buf,
    struct hound_record *record)
{
    const struct chan_parse_desc *desc;
    float *data;
    float f;
    size_t i;

    record->data = drv_alloc(entry->data_size);
    if (record->data == NULL) {
        return HOUND_OOM;
    }
    data = (__typeof__(*data) *) record->data;

    record->size = entry->data_size;
    record->data_id = entry->id;

    for (i = 0; i < entry->num_channels; ++i) {
        desc = &entry->channels[i];
        /*
         * Copy sensor data, with the timestamp channel special-cased, since we put
         * it into the timestamp field instead of the data field.
         */
        f = desc->copy_func_float(
            &buf[desc->index],
            desc->shift,
            desc->mask);
        data[i] = f * desc->scale;
    }

    return HOUND_OK;
}

static
hound_err iio_parse(
    uint8_t *buf,
    size_t *bytes,
    struct hound_record *records,
    size_t *out_record_count)
{
    const struct iio_ctx *ctx;
    uint_fast64_t epoch_ns;
    hound_err err;
    size_t i;
    size_t j;
    const uint8_t *pos;
    struct hound_record *record;
    size_t record_count;
    const struct chan_parse_desc *timestamp_desc;
    struct timespec ts;
    size_t scan_count;

    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(bytes);
    XASSERT_GT(*bytes, 0);
    XASSERT_NOT_NULL(records);

    ctx = drv_ctx();
    XASSERT_NOT_NULL(ctx);

    scan_count = *bytes / ctx->scan_size;
    record_count = scan_count * ctx->num_entries;
    if (record_count > HOUND_DRIVER_MAX_RECORDS) {
        record_count = HOUND_DRIVER_MAX_RECORDS;
        scan_count = record_count / ctx->num_entries;
    }

    /* IIO should not provide partial scans. */
    XASSERT_EQ(*bytes % ctx->scan_size, 0);

    record = records;
    pos = buf;
    err = HOUND_OK;
    timestamp_desc = &ctx->timestamp_channel;
    for (i = 0; i < scan_count; ++i) {
        /* Process a scan. */
        timestamp_desc->copy_func(
            (uint8_t *) &epoch_ns,
            &buf[timestamp_desc->index],
            timestamp_desc->shift,
            timestamp_desc->mask);
        ts.tv_sec = epoch_ns / NSEC_PER_SEC;
        ts.tv_nsec = epoch_ns % NSEC_PER_SEC;

        for (j = 0; j < ctx->num_entries; ++j) {
            /* Process a record from several channels. */
            err = iio_make_record(
                &ctx->entries[j],
                pos,
                record);
            if (err != HOUND_OK) {
                if (record == records) {
                    err = HOUND_OOM;
                }
                else {
                    /*
                     * Although this means the system is in trouble, the records we
                     * produced are still OK. The next call will likely fail though.
                     */
                    err = HOUND_OK;
                }
                break;
            };

            record->timestamp = ts;
            ++record;
        }

        pos += ctx->scan_size;
        *bytes -= ctx->scan_size;
    }

    *out_record_count = record - records;

    return err;
}

static
hound_err iio_next(UNUSED hound_data_id id)
{
    /* We don't support one-shot data (only periodic). */
    return HOUND_DRIVER_UNSUPPORTED;
}

static
hound_err iio_reset(void *data)
{
    iio_destroy();
    iio_init(data);

    return HOUND_OK;
}

static struct driver_ops iio_driver = {
    .init = iio_init,
    .destroy = iio_destroy,
    .reset = iio_reset,
    .device_name = iio_device_name,
    .datadesc = iio_datadesc,
    .setdata = iio_setdata,
    .parse = iio_parse,
    .start = iio_start,
    .next = iio_next,
    .stop = iio_stop
};

PUBLIC_API
hound_err hound_register_iio_driver(
    const char *schema_base,
    const struct hound_iio_driver_init *init)
{
    if (init == NULL) {
        return HOUND_NULL_VAL;
    }

    /* The compiler can't verify it, but we won't change dev. */
    return driver_register(init->dev, &iio_driver, schema_base, (void *) init);
}
