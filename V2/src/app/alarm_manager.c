/**
 * @file alarm_manager.c
 * @brief [P2-M7] [P2-M8] Alarm Manager with Atomic Write config persistence and Buzzer Pulsing Controller
 * @author PHUC TAI
 */

#include "alarm_manager.h"
#include "../../include/smartclock_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define DEFAULT_ALARM_HOUR      7
#define DEFAULT_ALARM_MINUTE    0

int alarm_manager_load_config(const char *config_path, alarm_config_t *out_cfg)
{
    if (!config_path || !out_cfg)
        return -EINVAL;

    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        /* If file does not exist, initialize with default disabled values */
        out_cfg->hour = DEFAULT_ALARM_HOUR;
        out_cfg->minute = DEFAULT_ALARM_MINUTE;
        out_cfg->enabled = false;
        return -ENOENT;
    }

    char line[64];
    int hour = DEFAULT_ALARM_HOUR, min = DEFAULT_ALARM_MINUTE, en = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "hour=%d", &hour) == 1) continue;
        if (sscanf(line, "minute=%d", &min) == 1) continue;
        if (sscanf(line, "enabled=%d", &en) == 1) continue;
    }

    fclose(fp);

    out_cfg->hour = (hour >= 0 && hour <= 23) ? hour : DEFAULT_ALARM_HOUR;
    out_cfg->minute = (min >= 0 && min <= 59) ? min : DEFAULT_ALARM_MINUTE;
    out_cfg->enabled = (en != 0);

    printf("[alarm_manager] Loaded config: %02d:%02d (Enabled: %d)\n",
           out_cfg->hour, out_cfg->minute, out_cfg->enabled);

    return 0;
}

/* [P2-M7] Save alarm configuration to persistent storage using Atomic File Write Pattern (.tmp -> fsync -> rename) */
int alarm_manager_save_config_atomic(const char *config_path, const alarm_config_t *cfg)
{
    if (!config_path || !cfg)
        return -EINVAL;

    /* 1. Open temporary file */
    FILE *fp = fopen(ALARM_CONFIG_TMP_FILE, "w");
    if (!fp) {
        perror("alarm_manager: Failed to open temporary config file");
        return -errno;
    }

    /* 2. Write key-value configuration */
    fprintf(fp, "hour=%d\nminute=%d\nenabled=%d\n",
            cfg->hour, cfg->minute, cfg->enabled ? 1 : 0);

    /* 3. Flush userspace buffer and commit to physical storage */
    fflush(fp);
    if (fsync(fileno(fp)) < 0) {
        perror("alarm_manager: fsync failed");
        fclose(fp);
        unlink(ALARM_CONFIG_TMP_FILE);
        return -errno;
    }

    fclose(fp);

    /* 4. Atomic Rename to replace active config */
    if (rename(ALARM_CONFIG_TMP_FILE, config_path) < 0) {
        perror("alarm_manager: Failed to atomic rename config file");
        unlink(ALARM_CONFIG_TMP_FILE);
        return -errno;
    }

    printf("[alarm_manager] Successfully saved alarm config atomically to %s\n", config_path);
    return 0;
}

/* Helper to write state to a specific path */
static bool write_state_to_file(const char *path, const struct tm *tm_info, bool silenced)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return false;

    fprintf(fp, "year=%d\nmonth=%d\nday=%d\nhour=%d\nminute=%d\nsilenced=%d\n",
            tm_info->tm_year + 1900,
            tm_info->tm_mon + 1,
            tm_info->tm_mday,
            tm_info->tm_hour,
            tm_info->tm_min,
            silenced ? 1 : 0);

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    return true;
}

/* Helper to read and verify handled state from a specific path */
static bool check_state_from_file(const char *path, const struct tm *tm_info)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    char line[64];
    int year = 0, month = 0, day = 0, hour = -1, min = -1, silenced = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "year=%d", &year) == 1) continue;
        if (sscanf(line, "month=%d", &month) == 1) continue;
        if (sscanf(line, "day=%d", &day) == 1) continue;
        if (sscanf(line, "hour=%d", &hour) == 1) continue;
        if (sscanf(line, "minute=%d", &min) == 1) continue;
        if (sscanf(line, "silenced=%d", &silenced) == 1) continue;
    }
    fclose(fp);

    if (year == (tm_info->tm_year + 1900) &&
        month == (tm_info->tm_mon + 1) &&
        day == tm_info->tm_mday &&
        hour == tm_info->tm_hour &&
        min == tm_info->tm_min) {
        return true;
    }

    return false;
}

/* Record alarm trigger or silence event into persistent state */
void alarm_manager_record_state(const struct tm *tm_info, bool silenced)
{
    if (!tm_info) return;

    mkdir("/etc/smartclock", 0755);

    /* Try primary persistent path first (/etc/smartclock/alarm.state) */
    if (!write_state_to_file(ALARM_STATE_FILE, tm_info, silenced)) {
        /* Fall back to /tmp if primary fails (e.g. non-root permissions or dev test) */
        write_state_to_file(ALARM_STATE_FALLBACK_FILE, tm_info, silenced);
    }
}

/* Check if current minute has already been handled (triggered/silenced) today */
bool alarm_manager_is_already_handled(const struct tm *tm_info)
{
    if (!tm_info) return false;

    if (check_state_from_file(ALARM_STATE_FILE, tm_info)) {
        return true;
    }

    return check_state_from_file(ALARM_STATE_FALLBACK_FILE, tm_info);
}

/* Clear persistent alarm state (called when user saves a new alarm on Web) */
void alarm_manager_clear_state(void)
{
    unlink(ALARM_STATE_FILE);
    unlink(ALARM_STATE_FALLBACK_FILE);
}

/* [P2-M8] Check current system time against configured alarm and trigger buzzer */
void alarm_manager_check(const struct tm *tm_info)
{
    if (!tm_info) return;

    pthread_mutex_lock(&g_state_mutex);

    /* Reset trigger tracker when minute advances */
    if (tm_info->tm_min != g_system_state.last_triggered_minute) {
        g_system_state.last_triggered_minute = -1;
    }

    alarm_config_t cfg = g_system_state.alarm_config;
    bool is_currently_ringing = g_system_state.alarm_ringing;

    if (!cfg.enabled || is_currently_ringing) {
        pthread_mutex_unlock(&g_state_mutex);
        return;
    }

    /* Check if current hour & minute match alarm schedule */
    if (tm_info->tm_hour == cfg.hour && tm_info->tm_min == cfg.minute) {
        if (g_system_state.last_triggered_minute != tm_info->tm_min) {
            /* Prevent re-triggering if already handled (silenced or fired) in this exact minute */
            if (alarm_manager_is_already_handled(tm_info)) {
                g_system_state.last_triggered_minute = tm_info->tm_min;
                pthread_mutex_unlock(&g_state_mutex);
                return;
            }

            g_system_state.last_triggered_minute = tm_info->tm_min;
            g_system_state.alarm_ringing = true;
            alarm_manager_record_state(tm_info, false);
            pthread_cond_broadcast(&g_state_cond);

            printf("[alarm_manager] ALARM TRIGGERED! Time: %02d:%02d:00\n",
                   tm_info->tm_hour, tm_info->tm_min);
        }
    }

    pthread_mutex_unlock(&g_state_mutex);
}

void *buzzer_thread_func(void *arg)
{
    (void)arg;
    int bz_fd = -1;

    printf("[buzzer_thread] Buzzer controller thread started.\n");

    while (1) {
        pthread_mutex_lock(&g_state_mutex);
        /* Block and sleep on condition variable when idle, avoiding continuous polling */
        while (g_system_state.running && !g_system_state.alarm_ringing) {
            if (bz_fd >= 0) {
                if (write(bz_fd, "0", 1) < 0) perror("buzzer write off");
                close(bz_fd);
                bz_fd = -1;
            }
            pthread_cond_wait(&g_state_cond, &g_state_mutex);
        }

        bool is_running = g_system_state.running;
        bool should_ring = g_system_state.alarm_ringing;
        pthread_mutex_unlock(&g_state_mutex);

        if (!is_running) {
            break;
        }

        if (should_ring) {
            /* Open character driver if not already opened */
            if (bz_fd < 0) {
                bz_fd = open(BUZZER_DEV_PATH, O_WRONLY);
                if (bz_fd < 0) {
                    perror("buzzer_thread: Failed to open buzzer driver");
                    usleep(500 * 1000);
                    continue;
                }
            }

            /* Beep Pattern: 500ms ON / 500ms OFF */
            if (write(bz_fd, "1", 1) < 0) perror("buzzer write on");
            usleep(500 * 1000);

            /* Check again before sleeping low to respond quickly to button press */
            pthread_mutex_lock(&g_state_mutex);
            should_ring = g_system_state.alarm_ringing;
            pthread_mutex_unlock(&g_state_mutex);

            if (should_ring) {
                if (write(bz_fd, "0", 1) < 0) perror("buzzer write off");
                usleep(500 * 1000);
            }
        }
    }

    if (bz_fd >= 0) {
        if (write(bz_fd, "0", 1) < 0) perror("buzzer write off");
        close(bz_fd);
    }

    printf("[buzzer_thread] Thread safely terminated.\n");
    return NULL;
}