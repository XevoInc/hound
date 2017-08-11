/**
 * @file      counter.c
 * @brief     Unit test for the counter driver, which tests the basic I/O
 *            subsystem.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <hound/error.h>
#include <hound/driver.h>
#include <hound/driver/file.h>
#include <hound/hound.h>
#include <linux/limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define TESTFILE "data/testfile"
#define NS_PER_SEC (1e9)

extern struct hound_driver file_driver;

struct text {
    char *data;
    size_t index;
};

char *s_text;
void data_cb(struct hound_record *record, void *data)
{
	struct text *text;
    int ret;

    HOUND_ASSERT_NOT_NULL(record);
    HOUND_ASSERT_NOT_NULL(record->data);
    HOUND_ASSERT_GT(record->size, 0);
    HOUND_ASSERT_NOT_NULL(data);

	text = data;
    ret = memcmp(text->data + text->index, record->data, record->size);
    HOUND_ASSERT_EQ(ret, 0);
	text->index += record->size;
}

char *slurp_file(const char *filepath, size_t *count)
{
    ssize_t bytes;
    char *data;
    int fd;
    int ret;
    struct stat st;

    fd = open(filepath, 0, O_RDONLY);
    HOUND_ASSERT_NEQ(fd, -1);

    ret = fstat(fd, &st);
    HOUND_ASSERT_EQ(ret, 0);
    data = malloc(st.st_size);
    HOUND_ASSERT_NOT_NULL(data);

    bytes = 0;
    do {
        ret = read(fd, data + bytes, st.st_size - bytes);
        HOUND_ASSERT_GT(ret, 0);
        bytes += ret;
    } while (bytes < st.st_size);

    *count = st.st_size;

    close(fd);

    return data;
}

#include <stdio.h>

int main(int argc, const char **argv)
{
    struct hound_ctx *ctx;
    hound_err err;
	struct text text;
    size_t total_count;
    struct hound_data_rq data_rq =
        { .id = HOUND_DEVICE_ACCELEROMETER, .period_ns = 0 };
    struct hound_driver_file_init init = {
        .data_id = data_rq.id,
        .period_ns = data_rq.period_ns
    };
    struct hound_rq rq = {
        /*
         * Make the queue large to reduce the chance of overwriting the circular
         * buffer.
         */
        .queue_len = 100,
        .cb = data_cb,
		.cb_ctx = &text,
        .rq_list.len = 1,
        .rq_list.data = &data_rq
    };

    if (argc != 2) {
        fprintf(stderr, "Usage: file TESTFILE\n");
        exit(EXIT_FAILURE);
    }
    if (strnlen(argv[1], PATH_MAX) == PATH_MAX) {
        fprintf(stderr, "File argument is longer than PATH_MAX\n");
        exit(EXIT_FAILURE);
    }
    init.filepath = argv[1];

    err = hound_register_driver("/dev/filedrv", &file_driver, &init);
    HOUND_ASSERT_OK(err);

    err = hound_alloc_ctx(&ctx, &rq);
    HOUND_ASSERT_OK(err);

    err = hound_start(ctx);
    HOUND_ASSERT_OK(err);

    text.data = slurp_file(init.filepath, &total_count);
    text.index = 0;
    while (text.index < total_count) {
        err = hound_read(ctx, 1);
        HOUND_ASSERT_OK(err);
    }
    HOUND_ASSERT_EQ(text.index, total_count);
    free(s_text);

    err = hound_stop(ctx);
    HOUND_ASSERT_OK(err);

    err = hound_free_ctx(ctx);
    HOUND_ASSERT_OK(err);

    err = hound_unregister_driver("/dev/filedrv");
    HOUND_ASSERT_OK(err);

    return 0;
}
