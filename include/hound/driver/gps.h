/**
 * @file      gps.h
 * @brief     Public GPS driver header.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_DRIVER_GPS_H_
#define HOUND_DRIVER_GPS_H_

#include <hound/hound.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Records from the GPS driver are castable to this struct.
 *
 * These properties are taken from gpsd's struct gps_fix_t in gps.h and have the
 * same other than time_uncertainty, which is nanoseconds instead of seconds.
 */
struct gps_data {
    double time_uncertainty;
    double latitude;
    double latitude_uncertainty;
    double longitude;
    double longitude_uncertainty;
    double altitude;
    double altitude_uncertainty;
    double track;
    double track_uncertainty;
    double speed;
    double speed_uncertainty;
    double climb;
    double climb_uncertainty;
};

#ifdef __cplusplus
}
#endif

#endif /* HOUND_DRIVER_GPS_H_ */
