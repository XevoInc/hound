/**
 * @file      sim.h
 * @brief     Header for the OBD II simulator.
 *            back realistic-seeming responses.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_TEST_SIM_H_
#define HOUND_TEST_SIM_H_

#include <semaphore.h>
#include <stdbool.h>
#include <yobd/yobd.h>

struct thread_ctx {
    const char *iface;
    const char *schema_file;
    struct yobd_ctx *yobd_ctx;
    int fd;
    sem_t ready;
};

void *run_sim(void *data);

#endif /* HOUND_TEST_SIM_H_ */
