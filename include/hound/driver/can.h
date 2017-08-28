/**
 * @file      can.h
 * @brief     Public CAN driver header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_DRIVER_CAN_H_
#define HOUND_DRIVER_CAN_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <hound/hound.h>
/*
 * This breaks the normal alphabetized-headers rule; <sys/socket.h> must come
 * first or <linux/if.h> will cause a compile error.
 */
#include <sys/socket.h>
#include <linux/if.h>

struct hound_can_driver_init {
    char iface[IFNAMSIZ];
    int recv_own_msg;
    uint32_t tx_count;
    struct can_frame *tx_frames;
};

hound_err hound_register_can_driver(struct hound_can_driver_init *init);

#ifdef __cplusplus
}
#endif

#endif /* HOUND_DRIVER_CAN_H_ */
