/**
 * @file      id.h
 * @brief     Test driver data ID definitions.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_TEST_ID_H_
#define HOUND_TEST_ID_H_

#include <hound/hound.h>

/*
 * Use the prefix 0xffffff to minimize the chance of collisions with
 * production data IDs.
 */
#define HOUND_DATA_COUNTER ((hound_data_id) 0xffffff00)
#define HOUND_DATA_FILE ((hound_data_id) 0xffffff01)
#define HOUND_DATA_NOP1 ((hound_data_id) 0xffffff02)
#define HOUND_DATA_NOP2 ((hound_data_id) 0xffffff03)

#endif /* HOUND_TEST_ID_H_ */
