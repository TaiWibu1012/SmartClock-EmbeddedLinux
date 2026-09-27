/**
 * @file smartconfig.h
 * @brief Soft AP and Station mode management with 20-second fallback mechanism
 * @author PHUC TAI 
 */

#ifndef _SMARTCONFIG_H_
#define _SMARTCONFIG_H_

#include "system_state.h"

#define WPA_CONF_FILE           "/etc/wpa_supplicant/wpa_supplicant.conf"
#define WIFI_CONNECT_TIMEOUT_S  20
#define WIFI_CHECK_INTERVAL_S   2

typedef enum {
    REQ_NET_NONE = 0,
    REQ_NET_START_SOFTAP,
    REQ_NET_CONNECT_STATION
} net_request_t;

/**
 * @brief Asynchronously request switching to Soft AP mode
 */
void smartconfig_trigger_softap(void);

/**
 * @brief Asynchronously request connecting to a Wi-Fi Station
 * @param ssid Target Wi-Fi SSID
 * @param password Target Wi-Fi Password
 */
void smartconfig_trigger_station(const char *ssid, const char *password);

/**
 * @brief Worker thread for managing network transitions and background polling
 * @param arg Unused
 * @return NULL on termination
 */
void *smartconfig_thread_func(void *arg);

/**
 * @brief Wake up smartconfig worker thread (used during clean system shutdown)
 */
void smartconfig_wakeup(void);

#endif /* _SMARTCONFIG_H_ */