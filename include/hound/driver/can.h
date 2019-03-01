/**
 * @file      can.h
 * @brief     Public CAN driver header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_DRIVER_CAN_H_
#define HOUND_DRIVER_CAN_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <linux/can.h>
#include <hound/hound.h>
/*
 * This breaks the normal alphabetized-headers rule; <sys/socket.h> must come
 * first or <linux/if.h> will cause a compile error.
 */
#include <sys/socket.h>
#include <linux/if.h>

struct hound_can_driver_init {
    char iface[IFNAMSIZ];
    canid_t rx_can_id;
    uint32_t tx_count;
    struct can_frame *tx_frames;
};

hound_err hound_register_can_driver(
    const char *schema_base,
    struct hound_can_driver_init *init);

#ifdef __cplusplus
}
#endif

#endif /* HOUND_DRIVER_CAN_H_ */
