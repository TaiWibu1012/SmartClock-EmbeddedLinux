/**
 * @file weather_screen.h
 * @brief Weather display screen controller & HTTP client interface
 * @author PHUC TAI
 */

#ifndef _WEATHER_SCREEN_H_
#define _WEATHER_SCREEN_H_

#include "system_state.h"
#include "../../include/smartclock_common.h"

#define WEATHER_SERVER_IP       LOCAL_LOOPBACK_IP
#define WEATHER_SERVER_PORT     DEFAULT_HTTP_PORT
#define WEATHER_REQUEST_PATH    "/weather"

#define WEATHER_TIMEOUT_SEC     5     /* 5 seconds non-blocking network timeout */
#define WEATHER_REFRESH_SEC     300   /* 5 minutes refresh interval */

void *weather_thread_func(void *arg);

/* Synchronized prototype: 2 parameters */
void render_weather_ui(const weather_data_t *data, net_mode_t net_mode);

#endif /* _WEATHER_SCREEN_H_ */