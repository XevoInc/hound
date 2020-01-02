/**
 * @file      config.h
 * @brief     Header for config parsing routines.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 *
 */

#ifndef HOUND_PRIVATE_PARSE_CONFIG_H_
#define HOUND_PRIVATE_PARSE_CONFIG_H_

#include <hound/hound.h>

hound_err parse_config(const char *config_path, const char *schema_base);

#endif /* HOUND_PRIVATE_PARSE_CONFIG_H_ */
