/**
 * @file      io.h
 * @brief     I/O subsystem header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_IO_H_
#define HOUND_PRIVATE_IO_H_

#include <hound/hound.h>
#include <hound-private/driver.h>
#include <hound-private/queue.h>
#include <hound-private/util.h>

/* Forward declaration. */
struct driver;

void io_init(void);
void io_destroy(void);

hound_err io_add_fd(
    int fd,
    struct driver *drv,
    const struct hound_data_rq *rqs,
    size_t rqs_len,
    struct queue *queue);

void io_remove_fd(int fd);

PUBLIC_API
hound_err io_default_push(
    short events,
    short *next_events,
    hound_data_period poll_time,
    bool *timeout_enabled,
    hound_data_period *timeout);

PUBLIC_API
hound_err io_default_pull(
    short events,
    short *next_events,
    hound_data_period poll_time,
    bool *timeout_enabled,
    hound_data_period *timeout);

PUBLIC_API
void io_push_records(struct hound_record *records, size_t count);

hound_err io_add_queue(
    int fd,
    const struct hound_data_rq *rqs,
    size_t rqs_len,
    struct queue *queue);
void io_remove_queue(
    int fd,
    const struct hound_data_rq *rqs,
    size_t rqs_len,
    struct queue *queue);
hound_err io_modify_queue(
    int fd,
    const struct hound_data_rq *old_rqs,
    size_t old_rqs_len,
    const struct hound_data_rq *new_rqs,
    size_t new_rqs_len,
    struct queue *queue);

#endif /* HOUND_PRIVATE_IO_H_ */
