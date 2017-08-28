/**
 * @file      file.h
 * @brief     Public file driver header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_DRIVER_FILE_H_
#define HOUND_DRIVER_FILE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <hound/hound.h>

hound_err hound_register_file_driver(const char *filepath, hound_data_id id);

#ifdef __cplusplus
}
#endif

#endif /* HOUND_DRIVER_FILE_H_ */
