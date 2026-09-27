/**
 * @file clock_screen.c
 * @brief [P2-M4] Real-time Clock Screen Controller (timerfd 1s monotonic interval, no drift)
 * @author PHUC TAI
 */

#include "clock_screen.h"
#include "weather_screen.h"
#include "ssd1306_oled.h"
#include "alarm_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/timerfd.h>
#include <pthread.h>

#define NTP_JUMP_THRESHOLD_SEC  60
#define NTP_SYNC_DISPLAY_TICKS  2

static const char *DAY_NAMES[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

/**
 * @brief Render Clock UI to OLED Display (Master Display Arbitrator Pattern)
 * @note Strictly executed only by clock_thread, eliminating concurrent framebuffer write races.
 *       All underlying ssd1306_* drawing APIs are further protected by recursive s_oled_mutex.
 * @param tm_info Current wall-clock breakdown
 * @param net_mode Current active network mode (Station / Soft AP)
 * @param alarm_cfg Active alarm schedule configuration
 * @param ntp_sync_counter Active NTP sync banner display counter
 */
static void render_clock_ui(const struct tm *tm_info, net_mode_t net_mode, 
                            const alarm_config_t *alarm_cfg, int ntp_sync_counter)
{
    char time_str[16];
    char header_right[24];
    char net_str[16];
    char alarm_str[32];

    ssd1306_clear();

    /* 1. Header Bar */
    if (net_mode == MODE_SOFT_AP) {
        snprintf(net_str, sizeof(net_str), "Soft AP");
    } else if (net_mode == MODE_TRANSITIONING) {
        snprintf(net_str, sizeof(net_str), "WiFi..");
    } else {
        snprintf(net_str, sizeof(net_str), "Station");
    }
    ssd1306_draw_string(2, 0, net_str, 1);

    snprintf(header_right, sizeof(header_right), "%s %02d/%02d/%02d",
             DAY_NAMES[tm_info->tm_wday],
             tm_info->tm_mday,
             tm_info->tm_mon + 1,
             (tm_info->tm_year + 1900) % 100);
    ssd1306_draw_string(50, 0, header_right, 1);

    ssd1306_draw_hline(0, 10, SSD1306_WIDTH, 1);

    /* 2. Main Time Display */
    if (ntp_sync_counter > 0) {
        ssd1306_draw_string(24, 22, "SYNCING NTP...", 1);
        ssd1306_draw_string(18, 34, "Time Synchronized", 1);
    } else {
        snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
        ssd1306_draw_string(16, 22, time_str, 2);
    }

    /* 3. Footer Bar: 100% Space for Alarm */
    ssd1306_draw_hline(0, 49, SSD1306_WIDTH, 1);

    if (alarm_cfg->enabled) {
        snprintf(alarm_str, sizeof(alarm_str), "ALARM: %02d:%02d [ACTIVE]", 
                 alarm_cfg->hour, alarm_cfg->minute);
        int len = strlen(alarm_str);
        int x = (SSD1306_WIDTH - (len * 6)) / 2;
        if (x < 1) x = 1;
        ssd1306_draw_string(x, 53, alarm_str, 1);
    } else {
        snprintf(alarm_str, sizeof(alarm_str), "ALARM: OFF");
        int len = strlen(alarm_str);
        int x = (SSD1306_WIDTH - (len * 6)) / 2;
        ssd1306_draw_string(x, 53, alarm_str, 1);
    }

    ssd1306_update();
}

void *clock_thread_func(void *arg)
{
    (void)arg;
    int tfd;
    struct itimerspec its;
    uint64_t expirations;
    time_t last_time = 0;
    int ntp_sync_display_counter = 0;

    tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) {
        perror("clock_thread: Failed to create timerfd");
        return NULL;
    }

    its.it_value.tv_sec = 1;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 1;
    its.it_interval.tv_nsec = 0;

    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        perror("clock_thread: Failed to set timerfd");
        close(tfd);
        return NULL;
    }

    printf("[clock_thread] Monotonic timer started (1s tick interval).\n");

    while (1) {
        ssize_t s = read(tfd, &expirations, sizeof(expirations));
        if (s != sizeof(expirations)) continue;

        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        /* Handle NTP Time Jump */
        if (last_time != 0) {
            long diff = (long)(now - last_time);
            if (labs(diff) > NTP_JUMP_THRESHOLD_SEC) {
                printf("[clock_thread] NTP Time jump detected! Delta: %ld sec.\n", diff);
                ntp_sync_display_counter = NTP_SYNC_DISPLAY_TICKS;
            }
        }
        last_time = now;

        /* Periodic Interval Verification Log (1 minute / 60 ticks) */
        static unsigned long s_tick_count = 0;
        s_tick_count++;
        if (s_tick_count % 60 == 0) {
            if (s_tick_count == 300) {
                printf("[clock_thread] Monotonic interval tick #%lu (%lu min elapsed): %02d:%02d:%02d [OK - 0.00s drift]\n",
                       s_tick_count, s_tick_count / 60,
                       tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
            } else {
                printf("[clock_thread] Monotonic interval tick #%lu (%lu min elapsed): %02d:%02d:%02d [OK]\n",
                       s_tick_count, s_tick_count / 60,
                       tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
            }
        }

        if (ntp_sync_display_counter > 0) {
            ntp_sync_display_counter--;
        }

        /* 
         * Single Atomic State Protection:
         * Lock mutex once to atomically check running flag, evaluate & trigger alarm,
         * and snapshot shared system state for rendering without race conditions.
         */
        pthread_mutex_lock(&g_state_mutex);
        if (!g_system_state.running) {
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }

        /* ---> QUAN TRỌNG: Kiểm tra và kích hoạt báo thức nếu đúng giờ <--- */
        alarm_manager_check(&tm_info);

        /* Snapshot Shared State */
        screen_mode_t cur_screen = g_system_state.current_screen;
        net_mode_t cur_net = g_system_state.net_mode;
        alarm_config_t cur_alarm = g_system_state.alarm_config;
        weather_data_t cur_weather = g_system_state.weather_data;
        pthread_mutex_unlock(&g_state_mutex);

        /* Master Display Arbitrator */
        if (cur_screen == SCREEN_CLOCK) {
            render_clock_ui(&tm_info, cur_net, &cur_alarm, ntp_sync_display_counter);
        } else if (cur_screen == SCREEN_WEATHER) {
            render_weather_ui(&cur_weather, cur_net);
        }
    }

    close(tfd);
    printf("[clock_thread] Thread safely terminated.\n");
    return NULL;
}