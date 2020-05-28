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

/*
 * Push to back, pop from front. Back is calculated implicitly as front + len
 * with wraparound.
 */
struct queue {
    pthread_mutex_t mutex;
    pthread_cond_t ready_cond;
    bool interrupt;
    size_t max_len;
    size_t len;
    size_t front;
    hound_seqno front_seqno;
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
    pthread_cond_init(&queue->ready_cond, NULL);
    queue->interrupt = false;
    queue->max_len = max_len;
    queue->len = 0;
    queue->front = 0;
    queue->front_seqno = 0;

    *out_queue = queue;

    return HOUND_OK;
}

static
void drain_until(struct queue *queue, size_t new_len)
{
    size_t back;
    size_t drain_count;
    size_t end;
    size_t end_records;
    size_t i;

    if (new_len >= queue->len) {
        return;
    }

    drain_count = queue->len - new_len;

    back = (queue->front + queue->len) % queue->max_len;
    if (back > queue->front) {
        /* Queue data is contiguous. */
        end = queue->front + drain_count;
        for (i = queue->front; i < end; ++i) {
            record_ref_dec(queue->data[i]);
        }
    }
    else {
        /* Queue data wraps around. */
        end_records = queue->max_len - queue->front;
        if (drain_count <= end_records) {
            end = queue->front + drain_count;
        }
        else {
            end = queue->max_len;
        }

        for (i = queue->front; i < end; ++i) {
            record_ref_dec(queue->data[i]);
        }

        if (drain_count > end_records) {
            /* More to drain at the start of the array. */
            end = drain_count - end_records;
            for (i = 0; i < end; ++i) {
                record_ref_dec(queue->data[i]);
            }
        }
    }

    queue->front = end % queue->max_len;
    queue->front_seqno += drain_count;
    queue->len = new_len;
}

static
void drain_nolock(struct queue *queue)
{
    drain_until(queue, 0);
}

static
bool is_contiguous(struct queue *queue)
{
    if (queue->len == queue->max_len) {
        return false;
    }

    return (queue->front + queue->len) <= queue->max_len;
}

static
void contract_queue(struct queue *queue, size_t new_max_len)
{
    size_t back;
    size_t count;
    size_t start;

    XASSERT_LT(new_max_len, queue->max_len);

    /* Trim the queue to match the new size. */
    drain_until(queue, new_max_len);

    /*
     * Calculate how many records will "fall off the end" after contraction, and
     * move these records to the front of the queue. If there are records at the
     * front of the queue, we also need to move these records forward.
     *
     * This code is tricky, as there are 4 cases, depending on the queue
     * structure and where we are truncating the queue. To clarify things a
     * little, bit we have ASCII art depicting the queue before and after
     * truncation. The legend for the art is as follows:
     *
     * |     indicates the start and end of queue->data
     * _     indicates empty space in the queue
     * -     indicates where we are truncating the queue
     * 1,2,3 indicate the queue items
     * f     indicates the front position of the queue
     * b     indicates the back position of the queue
     * -->   indicates how the queue will change after truncation
     */
    back = (queue->front + queue->len) % queue->max_len;
    if (is_contiguous(queue)) {
        /* Queue data is contiguous. */
        if (queue->front >= new_max_len) {
            /*
             * The entirety of the queue data needs to be moved, so front also
             * moves to the start of the array.
             *
             *         f  b
             * |_____-_123_|
             * -->
             *  f  b
             * |123__|
             */
            start = queue->front;
            count = back - queue->front;
            queue->front = 0;
        }
        else if (back > new_max_len) {
            /*
             * The new queue will cut through the currently-contiguous array.
             * Move the last part of the array to the start of the array, making
             * this into a wrap-around array.
             *
             *     f   b
             * |___12-3____|
             * -->
             *   b f
             * |3__12|
             *
             */
            start = new_max_len;
            count = back - new_max_len;
        }
        else {
            /*
             * Nothing falling off the end, so we're done.
             *
             *   f  b
             * |_123_-_____|
             * -->
             *   f  b
             * |_123_|
             *
             * */
            return;
        }
    }
    else {
        /* Queue data wraps around. */
        if (queue->front >= new_max_len) {
            /*
             * The new queue will completely remove the records at the end of
             * the array. Move these records to the start, making the array
             * contiguous.
             *
             *   b       f
             * |3____-___12|
             * -->
             *  f  b
             * |123__|
             *
             */
            start = queue->front;
            count = queue->max_len - queue->front;
            queue->front = 0;
        }
        else {
            /*
             * The new queue will cut through part of the records at the end of
             * the array. Move these records to the start, keeping the queue as
             * wrap-around.
             *
             *   b  f
             * |3___1-2____|
             * -->
             *    b f
             * |23__1|
             *
             */
            start = new_max_len;
            count = queue->len - back - (new_max_len - queue->front);
        }

        /*
         * We need to move the block of records at the start of the data to the
         * right so we can later move the "falling off" records to the start of
         * the array.
         */
        memmove(
            queue->data + count,
            queue->data,
            back * sizeof(*queue->data));
    }

    /*
     * Move the records that are about to fall off to the front of the array so
     * we will naturally wrap around to them when we contract the queue.
     */
    memcpy(
        queue->data,
        queue->data + start,
        count * sizeof(*queue->data));
}

static
void expand_queue(struct queue *queue, size_t new_max_len)
{
    size_t back;
    size_t diff;

    XASSERT_GT(new_max_len, queue->len);

    if (is_contiguous(queue)) {
        /*
         * See above for ASCI art legend.
         *
         * Contiguous queue, so nothing to do here.
         *   f  b
         * |_123_|
         * -->
         *   f  b
         * |_123______|
         */
        return;
    }

    /*
     * The queue is not contiguous, so move the data that previously wrapped
     * around from the start of the array to the new array end.
     */
    back = (queue->front + queue->len) % queue->max_len;
    diff = new_max_len - queue->max_len;
    if (back <= diff) {
        /*
         * We can fit all the data from the start of the array at the end of the
         * new array.
         *
         * Array size increasing from 5 to 7.
         *
         *    b f
         * |23__1|
         * -->
         *      f  b
         * |____123|
         */
        memcpy(
            queue->data + queue->len,
            queue->data,
            back * sizeof(*queue->data));
    }
    else {
        /*
         * We *cannot* fit all the data from the start of the array at the end of the
         * new array. Copy as much as we can and then move everything past that
         * to the start of the array.
         *
         * Array size increasing from 6 to 7.
         *
         *    b  f
         * |23___1|
         * -->
         *   b   f
         * |3____12|
         */
        memcpy(
            queue->data + queue->len,
            queue->data,
            diff * sizeof(*queue->data));
        memmove(
            queue->data,
            queue->data + diff,
            (back - diff) * sizeof(*queue->data));
    }
}

hound_err queue_resize(struct queue **out_queue, size_t max_len, bool flush)
{
    hound_err err;
    struct queue *queue;

    queue = *out_queue;
    XASSERT_NOT_NULL(queue);

    pthread_mutex_lock(&queue->mutex);

    if (flush) {
        drain_nolock(queue);
        /*
         * Reset the front pointer to make sure it's in bounds of the new
         * queue length.
         */
        queue->front = 0;
    }
    else if (max_len < queue->max_len) {
        contract_queue(queue, max_len);
    }

    if (max_len != queue->max_len) {
        queue = realloc(
            queue,
            sizeof(*queue) + sizeof(*queue->data)*max_len);
        if (queue == NULL) {
            err = HOUND_OOM;
            goto out;
        }
        *out_queue = queue;

        if (max_len > queue->max_len) {
            expand_queue(queue, max_len);
        }
    }

    queue->max_len = max_len;
    err = HOUND_OK;

out:
    pthread_mutex_unlock(&queue->mutex);
    return err;
}

void queue_destroy(struct queue *queue)
{
    XASSERT_NOT_NULL(queue);

    queue_drain(queue);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->ready_cond);
    free(queue);
}

void queue_interrupt(struct queue *queue)
{
    int err;

    pthread_mutex_lock(&queue->mutex);
    queue->interrupt = true;
    err = pthread_cond_signal(&queue->ready_cond);
    XASSERT_EQ(err, 0);
    pthread_mutex_unlock(&queue->mutex);
}

static
void pop_helper(
    struct queue *queue,
    struct record_info **buf,
    hound_seqno *first_seqno,
    size_t records)
{
    size_t right_records;

    XASSERT_LTE(records, queue->max_len);

    right_records = queue->max_len - queue->front;
    if (records < right_records) {
        /* All records we need are between [front, back]. */
        memcpy(buf, queue->data + queue->front, records * sizeof(*buf));
        queue->front += records;
    }
    else {
        /*
         * Records wrap around, so we need records from both [front, end] and
         * [start, back].
         *
         * Note that if records == right_records, then the second memcpy will be
         * a no-op and the front pointer will be correctly wrapped.
         */
        memcpy(buf, queue->data + queue->front, right_records * sizeof(*buf));
        memcpy(
            buf + right_records,
            queue->data,
            (records - right_records) * sizeof(*buf));
        queue->front = records - right_records;
    }
    queue->len -= records;
    *first_seqno = queue->front_seqno;
    queue->front_seqno += records;
}

static
size_t pop_bytes(
    struct queue *queue,
    struct record_info **buf,
    size_t bytes,
    hound_seqno *first_seqno,
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

    pop_helper(queue, buf, first_seqno, records);
    *out_records = records;

    return bytes - remainder;
}

static
size_t pop_records(
    struct queue *queue,
    struct record_info **buf,
    hound_seqno *first_seqno,
    size_t records)
{
    size_t target;

    /* Clamp the number of items to pop at the queue length. */
    target = min(records, queue->len);
    pop_helper(queue, buf, first_seqno, target);

    return target;
}


void queue_push(struct queue *queue, struct record_info *rec)
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
         * Overflow. Increment front so we remove the oldest entry, preserving
         * our max queue length.
         */
        XASSERT_EQ(queue->len, queue->max_len);
        XASSERT_EQ(queue->front, back);
        tmp = queue->data[queue->front];
        queue->front = (queue->front + 1) % queue->max_len;
        ++queue->front_seqno;
    }

    queue->data[back] = rec;

    err = pthread_cond_signal(&queue->ready_cond);
    XASSERT_EQ(err, 0);
    pthread_mutex_unlock(&queue->mutex);

    if (tmp != NULL) {
        record_ref_dec(tmp);
    }
}

size_t queue_pop_records_sync(
    struct queue *queue,
    struct record_info **buf,
    size_t records,
    hound_seqno *first_seqno,
    bool *interrupt)
{
    size_t count;
    hound_err err;
    hound_seqno *seqno;
    hound_seqno tmp;

    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(buf);

    count = 0;
    *interrupt = false;
    pthread_mutex_lock(&queue->mutex);
    do {
        /* TODO: Possible optimization: wake up only when n records/bytes are
         * ready, rather than when 1 is ready. Probably would need to use a heap
         * structure for this, to always wait for the smallest next wakeup
         * target. */
        while (queue->len < records && !queue->interrupt) {
            err = pthread_cond_wait(&queue->ready_cond, &queue->mutex);
            XASSERT_EQ(err, 0);
        }
        if (queue->interrupt) {
            *interrupt = true;
            queue->interrupt = false;
            break;
        }

        /*
         * We want to return the first sequence number for the entire buffer, so
         * if we call pop_records more than once, just throw away the later
         * values.
         */
        if (count == 0) {
            seqno = first_seqno;
        }
        else {
            seqno = &tmp;
        }

        count += pop_records(queue, buf + count, seqno, records - count);
    } while (count < records);

    pthread_mutex_unlock(&queue->mutex);

    return count;
}

size_t queue_pop_bytes_nowait(
    struct queue *queue,
    struct record_info **buf,
    size_t bytes,
    hound_seqno *first_seqno,
    size_t *records)
{
    size_t count;

    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(buf);
    XASSERT_NOT_NULL(records);

    pthread_mutex_lock(&queue->mutex);
    count = pop_bytes(queue, buf, bytes, first_seqno, records);
    pthread_mutex_unlock(&queue->mutex);

    return count;
}

size_t queue_pop_records_nowait(
    struct queue *queue,
    struct record_info **buf,
    hound_seqno *first_seqno,
    size_t records)
{
    size_t count;

    XASSERT_NOT_NULL(queue);
    XASSERT_NOT_NULL(buf);

    pthread_mutex_lock(&queue->mutex);
    count = pop_records(queue, buf, first_seqno, records);
    pthread_mutex_unlock(&queue->mutex);

    return count;
}

void queue_drain(struct queue *queue)
{
    XASSERT_NOT_NULL(queue);

    pthread_mutex_lock(&queue->mutex);
    drain_nolock(queue);
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
