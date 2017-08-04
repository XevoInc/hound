/**
 * @file      queue.c
 * @brief     Hound record queue implementation. The queue has a max length,
 *            which when exceeded will begin to overwrite the oldest item. It is
 *            thread-safe and blocks during the pop operation if the queue is
 *            empty. Thus it is intended for use in a producer-consumer
 *            scenario.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#include <hound/assert.h>
#include <hound_private/queue.h>
#include <hound_private/util.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct queue {
    pthread_mutex_t mutex;
    pthread_cond_t data_cond;
    size_t max_len;
    size_t len;
    size_t front;
    struct record_info *data[];
};

hound_err queue_alloc(
    struct queue **out_queue,
    size_t max_len)
{
    struct queue *queue;

    HOUND_ASSERT_NOT_NULL(out_queue);

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
    HOUND_ASSERT_NOT_NULL(queue);

    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->data_cond);
    free(queue);
}

size_t queue_pop_nolock(
    struct queue *queue,
    struct record_info **buf,
    size_t n)
{
    size_t right_records;
    size_t target;

    /* Clamp the number of items to pop at the queue length. */
    target = min(n, queue->len);

    right_records = queue->max_len - queue->front;
    if (target < right_records) {
        /* All records we need are between [front, back]. */
        memcpy(buf, queue->data + queue->front, target * sizeof(*buf));
        queue->front += target;
    }
    else {
        /* We need records from both [front, end] and [start, back]. */
        memcpy(buf, queue->data + queue->front, right_records * sizeof(*buf));
        memcpy(
            buf + right_records,
            queue->data,
            (target - right_records) * sizeof(*buf));
        queue->front = target - right_records;
    }
    queue->len -= target;

    return target;
}

void queue_push(
    struct queue *queue,
    struct record_info *rec)
{
    size_t back;
    hound_err err;

    HOUND_ASSERT_NOT_NULL(queue);
    HOUND_ASSERT_NOT_NULL(rec);

    pthread_mutex_lock(&queue->mutex);
    back = (queue->front + queue->len) % queue->max_len;
    queue->data[back] = rec;
    if (queue->len < queue->max_len) {
        ++queue->len;
    }
    err = pthread_cond_signal(&queue->data_cond);
    HOUND_ASSERT_EQ(err, 0);
    pthread_mutex_unlock(&queue->mutex);
}

void queue_pop(
   struct queue *queue,
   struct record_info **buf,
   size_t n)
{
    size_t count;
    hound_err err;

    HOUND_ASSERT_NOT_NULL(queue);
    HOUND_ASSERT_NOT_NULL(buf);

    count = 0;
    do {
        pthread_mutex_lock(&queue->mutex);
        /* TODO: Possible optimization: wake up only when n records are ready,
         * rather than when 1 is ready. Probably would need to use a heap
         * structure for this, to always wait for the smallest next wakeup
         * target. */
        while (queue->len < n) {
            err = pthread_cond_wait(&queue->data_cond, &queue->mutex);
            HOUND_ASSERT_EQ(err, 0);
        }

        count += queue_pop_nolock(queue, buf + count, n - count);
    } while (count < n);

    pthread_mutex_unlock(&queue->mutex);
}

size_t queue_pop_async(
    struct queue *queue,
    struct record_info **buf,
    size_t n)
{
    size_t count;

    HOUND_ASSERT_NOT_NULL(queue);
    HOUND_ASSERT_NOT_NULL(buf);

    pthread_mutex_lock(&queue->mutex);
    count = queue_pop_nolock(queue, buf, n);
    pthread_mutex_unlock(&queue->mutex);

    return count;
}

size_t queue_len(struct queue *queue)
{
    size_t len;

    HOUND_ASSERT_NOT_NULL(queue);

    pthread_mutex_lock(&queue->mutex);
    len = queue->len;
    pthread_mutex_unlock(&queue->mutex);

    return len;
}

size_t queue_max_len(struct queue *queue)
{
    size_t len;

    HOUND_ASSERT_NOT_NULL(queue);

    pthread_mutex_lock(&queue->mutex);
    len = queue->max_len;
    pthread_mutex_unlock(&queue->mutex);

    return len;
}
