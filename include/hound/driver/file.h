/**
 * @file      file.h
 * @brief     Private file driver functionality shared by multiple compilation units.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_DRIVER_FILE_H_
#define HOUND_DRIVER_FILE_H_

struct hound_driver_file_init {
    const char *filepath;
    hound_data_id data_id;
};

#endif /* HOUND_DRIVER_FILE_H_ */
