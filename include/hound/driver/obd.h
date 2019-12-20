/**
 * @file      OBD.h
 * @brief     Public OBD-II over CAN driver header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_DRIVER_OBD_H_
#define HOUND_DRIVER_OBD_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <hound/hound.h>
#include <linux/if.h>
#include <sys/socket.h>
#include <yobd/yobd.h>

void hound_obd_get_mode_pid(hound_data_id id, yobd_mode *mode, yobd_pid *pid);
void hound_obd_get_data_id(yobd_mode mode, yobd_pid pid, hound_data_id *id);

#ifdef __cplusplus
}
#endif

#endif /* HOUND_DRIVER_OBD_H_ */
