/**
 * @file      queue.h
 * @brief     Hound record queue header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_QUEUE_H_
#define HOUND_PRIVATE_QUEUE_H_

#include <hound/hound.h>
#include <hound_private/refcount.h>

struct record_info {
    atomic_refcount_val refcount;
    struct hound_record record;
};

struct queue;

typedef void (*queue_drain_cb)(struct record_info *rec);

hound_err queue_alloc(
    struct queue **queue,
    size_t max_len);
void queue_destroy(struct queue *queue);

void queue_push(
    struct queue *queue,
    struct record_info *rec);

void queue_pop_records_sync(
    struct queue *queue,
    struct record_info **buf,
    size_t n);

size_t queue_pop_bytes_async(
    struct queue *queue,
    struct record_info **buf,
    size_t bytes,
    size_t *records);

size_t queue_pop_records_async(
    struct queue *queue,
    struct record_info **buf,
    size_t records);

size_t queue_pop_nolock(
    struct queue *queue,
    struct record_info **buf,
    size_t n);

size_t queue_len(struct queue *queue);
size_t queue_max_len(struct queue *queue);

#endif /* HOUND_PRIVATE_QUEUE_H_ */
