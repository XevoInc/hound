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
#include <hound_private/driver.h>
#include <hound_private/queue.h>

void io_init(void);
void io_destroy(void);

hound_err io_add_fd(int fd, struct driver *drv);
void io_remove_fd(int fd);

hound_err io_add_queue(int fd, struct queue *queue);
void io_remove_queue(int fd, struct queue *queue);

#endif /* HOUND_PRIVATE_IO_H_ */
