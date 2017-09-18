/**
 * @file      valgrind.h
 * @brief     Valgrind header wrapper.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2017 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_PRIVATE_VALGRIND_H_
#define HOUND_PRIVATE_VALGRIND_H_

#include "config.h"

#ifdef CONFIG_HAVE_VALGRIND
#include <valgrind.h>
#else
#define RUNNING_ON_VALGRIND 0
#endif

#endif /* HOUND_PRIVATE_VALGRINd_H_ */
