/**
 * @file      queue.h
 * @brief     Hound record queue header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_QUEUE_H_
#define HOUND_PRIVATE_QUEUE_H_

/*
 * Forward declaration to avoid circular inclusion issues between queue.h and
 * driver.h.
 */
struct queue;

#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/refcount.h>

struct record_info {
    atomic_refcount_val refcount;
    struct hound_record record;
};

void record_ref_dec(struct record_info *info);

hound_err queue_alloc(
    struct queue **queue,
    size_t max_len);
hound_err queue_resize(struct queue *queue, size_t max_len, bool flush);

void queue_destroy(struct queue *queue);

void queue_interrupt(struct queue *queue);

void queue_push(
    struct queue *queue,
    struct record_info *rec);

size_t queue_pop_records(
    struct queue *queue,
    struct record_info **buf,
    size_t records,
    hound_seqno *first_seqno,
    bool *interrupt);

size_t queue_pop_bytes_nowait(
    struct queue *queue,
    struct record_info **buf,
    size_t bytes,
    hound_seqno *first_seqno,
    size_t *records);

size_t queue_pop_records_nowait(
    struct queue *queue,
    struct record_info **buf,
    hound_seqno *first_seqno,
    size_t records);

size_t queue_pop_nolock(
    struct queue *queue,
    struct record_info **buf,
    hound_seqno *first_seqno,
    size_t n);

void queue_drain(struct queue *queue);

size_t queue_len(struct queue *queue);
size_t queue_max_len(struct queue *queue);

#endif /* HOUND_PRIVATE_QUEUE_H_ */
