/**
 * @file system_state.h
 * @brief Global Shared System State and Mutex definitions
 * @author PHUC TAI
 */

#ifndef _SYSTEM_STATE_H_
#define _SYSTEM_STATE_H_

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* Screen Mode Enumeration */
typedef enum {
    SCREEN_CLOCK = 0,
    SCREEN_WEATHER
} screen_mode_t;

/* Network Mode Enumeration */
typedef enum {
    MODE_STATION = 0,
    MODE_SOFT_AP,
    MODE_TRANSITIONING
} net_mode_t;

/* Alarm Configuration Structure (1 Active Schedule) */
typedef struct {
    int hour;       /* 0 - 23 */
    int minute;     /* 0 - 59 */
    bool enabled;   /* true: Active, false: Disabled */
} alarm_config_t;

/* Weather Data Structure */
typedef struct {
    float temperature;
    char condition[32];      /* e.g., "Sunny", "Rainy", "Cloudy" */
    uint64_t last_update_ts; /* Monotonic timestamp in seconds */
    bool is_valid;           /* true: Data fresh, false: Net error / uninitialized */
} weather_data_t;

/* Wi-Fi Configuration Credentials */
typedef struct {
    char ssid[64];
    char password[64];
} wifi_creds_t;

/* Shared System State */
typedef struct {
    screen_mode_t   current_screen;
    net_mode_t      net_mode;
    bool            alarm_ringing;
    int             last_triggered_minute; /* Track last triggered minute to prevent repeated firing in the same minute */
    alarm_config_t  alarm_config;
    weather_data_t  weather_data;
    wifi_creds_t    pending_wifi;
    bool            force_weather_fetch;
    bool            running;
} system_state_t;

/* Global Mutex and Condition Variable protecting and signaling shared state */
extern pthread_mutex_t g_state_mutex;
extern pthread_cond_t  g_state_cond;
extern system_state_t  g_system_state;

/* State helper functions */
void system_state_init(void);
void system_state_destroy(void);

#endif /* _SYSTEM_STATE_H_ */