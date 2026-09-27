/**
 * @file alarm_manager.h
 * @brief Alarm manager and buzzer control thread with atomic configuration persistence
 * @author PHUC TAI
 */

#ifndef _ALARM_MANAGER_H_
#define _ALARM_MANAGER_H_

#include "system_state.h"
#include <time.h>

#define ALARM_CONFIG_FILE       "/etc/smartclock/alarm.conf"
#define ALARM_CONFIG_TMP_FILE   "/etc/smartclock/alarm.conf.tmp"
#define ALARM_STATE_FILE        "/etc/smartclock/alarm.state"
#define ALARM_STATE_FALLBACK_FILE "/tmp/smartclock_alarm.state"

#define DEFAULT_ALARM_HOUR      7
#define DEFAULT_ALARM_MINUTE    0

/**
 * @brief Load alarm configuration from persistent storage
 * @param config_path Path to alarm.conf
 * @param out_cfg Pointer to destination alarm_config_t
 * @return 0 on success, negative error code on failure
 */
int alarm_manager_load_config(const char *config_path, alarm_config_t *out_cfg);

/**
 * @brief Save alarm configuration using Enterprise Atomic File Write Pattern
 * @param config_path Path to alarm.conf
 * @param cfg Pointer to source alarm_config_t
 * @return 0 on success, negative error code on failure
 */
int alarm_manager_save_config_atomic(const char *config_path, const alarm_config_t *cfg);

/**
 * @brief Check current time against configured alarm and trigger ringing if matched
 * @param tm_info Current wall-clock breakdown from time(NULL)
 */
void alarm_manager_check(const struct tm *tm_info);

/**
 * @brief Record that the alarm for a specific minute was triggered or silenced to disk
 * @param tm_info Current timestamp breakdown
 * @param silenced true if silenced by user, false if triggered
 */
void alarm_manager_record_state(const struct tm *tm_info, bool silenced);

/**
 * @brief Check if the alarm for current minute has already been handled (triggered/silenced) today
 * @param tm_info Current timestamp breakdown
 * @return true if already handled, false otherwise
 */
bool alarm_manager_is_already_handled(const struct tm *tm_info);

/**
 * @brief Clear persisted alarm state (e.g. when user configures a new alarm schedule via Web)
 */
void alarm_manager_clear_state(void);

/**
 * @brief Worker thread for pulsing buzzer hardware when alarm_ringing is true
 * @param arg Unused
 * @return NULL on termination
 */
void *buzzer_thread_func(void *arg);

#endif /* _ALARM_MANAGER_H_ */