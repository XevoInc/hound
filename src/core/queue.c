/**
 * @file      queue.c
 * @brief     Hound record queue implementation. The queue has a max length,
 *            which when exceeded will begin to overwrite the oldest item. It is
 *            thread-safe and blocks during the pop operation if the queue is
 *            empty. Thus it is intended for use in a producer-consumer
 *            scenario.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#include <hound-private/error.h>
#include <hound-private/queue.h>
#include <hound-private/util.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DEQUEUE_BUF_SIZE (4096 / sizeof(struct hound_record_info *))

/*
 * Push to back, pop from front. Back is calculated implicitly as front + len
 * with wraparound.
 */
struct queue {
    pthread_mutex_t mutex;
    pthread_cond_t data_cond;
    size_t max_len;
    size_t len;
    size_t front;
    struct record_info *data[];
};

static
void free_record_info(struct record_info *info)
{
    drv_free(info->record.data);
    drv_free(info);
}

void record_ref_dec(struct record_info *info)
{
    refcount_val count;

    count = atomic_ref_dec(&info->refcount);
    if (count == 1) {
        /*
         * atomic_ref_dec returns the value *before* decrement, so this
         * means the refcount has now reached 0.
         */
        free_record_info(info);
    }
}

hound_err queue_alloc(
    struct queue **out_queue,
    size_t max_len)
{
    struct queue *queue;

    XASSERT_NOT_NULL(out_queue);

    queue = malloc(sizeof(*queue) + sizeof(*queue->data)*max_len);
    if (queue == NULL) {
        return HOUND_OOM;
    }

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->data_cond, NULL);
    queue->max_len = max_len;
    queue->len = 0;
    queue->front = 0;

    *out_queue = queue;

    return HOUND_OK;
}

void queue_destroy(struct queue *queue)
{
    XASSERT_NOT_NULL(queue);

    queue_drain(queue);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->data_cond);
    free(queue);
}

static
void pop_helper(
    struct queue *queue,
    struct record_info **buf,
    size_t records)
{
    size_t right_records;

    XASSERT_LTE(records, queue->max_len);

    right_records = queue->max_len - queue->front;
    if (records <= right_records) {
        /* All records we need are between [front, back]. */
        memcpy(buf, queue->data + queue->front, records * sizeof(*buf));
        queue->front += records;
    }
    else {
        /* We need records from both [front, end] and [start, back]. */
        memcpy(buf, queue->data + queue->front, right_records * sizeof(*buf));
        memcpy(
            buf + right_records,
            queue->data,
            (records - right_records) * sizeof(*buf));
        queue->front = records - right_records;
    }
    queue->len -= records;
}

static
size_t pop_bytes(
    struct queue *queue,
    struct record_info **buf,
    size_t bytes,
    size_t *out_records)
{
    size_t i;
    size_t remainder;
    size_t records;
    size_t size;

    records = 0;
    remainder = bytes;
    i = queue->front;
    while (true) {
        if (records == queue->len) {
            break;
        }

        size = queue->data[i]->record.size;
        if (remainder < size) {
            break;
        }
        remainder -= size;

        ++records;
        ++i;
        if (i == queue->max_len) {
            i = 0;
        }
    }

    pop_helper(queue, buf, records);
    *out_records = records;

    return bytes - remainder;
}

static
size_t pop_records(
    struct queue *queue,
    struct record_info **buf,
    size_t records)
{
    size_t target;

    /* Clamp the number of items to pop at the queue length. */
    target = min(records, queue->len);
    pop_helper(queue, buf, target);

    return target;
}


void queue_push(
    struct queue *queue,
    struct record_info *rec)
{
    size_t back;
    hound_err err;
    struct record_info *tmp;

    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(rec);

    pthread_mutex_lock(&queue->mutex);
    back = (queue->front + queue->len) % queue->max_len;
    if (queue->len < queue->max_len) {
        ++queue->len;
        tmp = NULL;
    }
    else {
        /*
         * Overflow. Increment front so it points to the oldest entry instead of
         * the one we just inserted.
         */
        queue->front = (queue->front + 1) % queue->max_len;
        tmp = queue->data[back];
    }
    queue->data[back] = rec;
    err = pthread_cond_signal(&queue->data_cond);
    XASSERT_EQ(err, 0);
    pthread_mutex_unlock(&queue->mutex);

    if (tmp != NULL) {
        record_ref_dec(tmp);
    }
}

void queue_pop_records_sync(
   struct queue *queue,
   struct record_info **buf,
   size_t records)
{
    size_t count;
    hound_err err;

    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(buf);

    count = 0;
    do {
        pthread_mutex_lock(&queue->mutex);
        /* TODO: Possible optimization: wake up only when n records/bytes are
         * ready, rather than when 1 is ready. Probably would need to use a heap
         * structure for this, to always wait for the smallest next wakeup
         * target. */
        while (queue->len < records) {
            err = pthread_cond_wait(&queue->data_cond, &queue->mutex);
            XASSERT_EQ(err, 0);
        }

        count += pop_records(queue, buf + count, records - count);
    } while (count < records);

    pthread_mutex_unlock(&queue->mutex);

}

size_t queue_pop_bytes_nowait(
    struct queue *queue,
    struct record_info **buf,
    size_t bytes,
    size_t *records)
{
    size_t count;

    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(records);

    pthread_mutex_lock(&queue->mutex);
    count = pop_bytes(queue, buf, bytes, records);
    pthread_mutex_unlock(&queue->mutex);

    return count;
}

size_t queue_pop_records_nowait(
    struct queue *queue,
    struct record_info **buf,
    size_t records)
{
    size_t count;

    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(buf);

    pthread_mutex_lock(&queue->mutex);
    count = pop_records(queue, buf, records);
    pthread_mutex_unlock(&queue->mutex);

    return count;
}

void queue_drain(struct queue *queue)
{
    size_t back;
    size_t i;

    pthread_mutex_lock(&queue->mutex);
    back = (queue->front + queue->len) % queue->max_len;
    if (back >= queue->front) {
        /* Queue data is contiguous. */
        for (i = queue->front; i < back; ++i) {
            record_ref_dec(queue->data[i]);
        }
    }
    else {
        /* Queue data wraps around. */
        for (i = queue->front; i < queue->max_len; ++i) {
            record_ref_dec(queue->data[i]);
        }
        for (i = 0; i < back; ++i) {
            record_ref_dec(queue->data[i]);
        }
    }

    queue->len = 0;
    pthread_mutex_unlock(&queue->mutex);
}

size_t queue_len(struct queue *queue)
{
    size_t len;

    XASSERT_NOT_NULL(queue);

    pthread_mutex_lock(&queue->mutex);
    len = queue->len;
    pthread_mutex_unlock(&queue->mutex);

    return len;
}

size_t queue_max_len(struct queue *queue)
{
    size_t len;

    XASSERT_NOT_NULL(queue);

    pthread_mutex_lock(&queue->mutex);
    len = queue->max_len;
    pthread_mutex_unlock(&queue->mutex);

    return len;
}
