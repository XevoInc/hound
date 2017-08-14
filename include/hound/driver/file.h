/**
 * @file      file.h
 * @brief     Public file driver header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_DRIVER_FILE_H_
#define HOUND_DRIVER_FILE_H_

#include <hound/hound.h>

hound_err hound_register_file_driver(const char *filepath, hound_data_id id);

#endif /* HOUND_DRIVER_FILE_H_ */
