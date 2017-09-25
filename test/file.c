/**
 * @file      file.c
 * @brief     Unit test for the file driver.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <hound/hound.h>
#include <hound_test/assert.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

extern hound_err hound_register_file_driver(
    const char *filepath, hound_data_id id);

struct text {
    char *data;
    size_t index;
};

void data_cb(struct hound_record *record, void *data)
{
    struct text *text;
    int ret;

    XASSERT_NOT_NULL(record);
    XASSERT_NOT_NULL(record->data);
    XASSERT_GT(record->size, 0);
    XASSERT_NOT_NULL(data);

    text = data;
    ret = memcmp(text->data + text->index, record->data, record->size);
    XASSERT_EQ(ret, 0);
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
    XASSERT_NEQ(fd, -1);

    ret = fstat(fd, &st);
    XASSERT_EQ(ret, 0);
    data = malloc(st.st_size);
    XASSERT_NOT_NULL(data);

    bytes = 0;
    do {
        ret = read(fd, data + bytes, st.st_size - bytes);
        XASSERT_GT(ret, 0);
        bytes += ret;
    } while (bytes < st.st_size);

    *count = st.st_size;

    close(fd);

    return data;
}

int main(int argc, const char **argv)
{
    struct hound_ctx *ctx;
    hound_err err;
    const char *filepath;
    struct text text;
    size_t total_count;
    struct hound_data_rq data_rq = { .id = HOUND_DEVICE_ACCELEROMETER };
    struct hound_rq rq = {
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
    filepath = argv[1];

    err = hound_register_file_driver(filepath, data_rq.id);
    XASSERT_OK(err);

    err = hound_alloc_ctx(&ctx, &rq);
    XASSERT_OK(err);

    err = hound_start(ctx);
    XASSERT_OK(err);

    text.data = slurp_file(filepath, &total_count);
    text.index = 0;
    while (text.index < total_count) {
        err = hound_read(ctx, 1);
        XASSERT_OK(err);
    }
    XASSERT_EQ(text.index, total_count);
    free(text.data);

    err = hound_stop(ctx);
    XASSERT_OK(err);

    err = hound_free_ctx(ctx);
    XASSERT_OK(err);

    err = hound_unregister_driver(filepath);
    XASSERT_OK(err);

    return 0;
}
