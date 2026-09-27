/**
 * @file main.c
 * @brief [P2-M6] [P2-M9] Main Application Entry Point & Button Priority Dispatcher (Short press screen toggle / Alarm silence)
 * @author PHUC TAI
 */

#include "system_state.h"
#include "ssd1306_oled.h"
#include "clock_screen.h"
#include "weather_screen.h"
#include "webserver.h"
#include "alarm_manager.h"
#include "smartconfig.h"
#include "../../include/smartclock_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <sys/wait.h>

#define BTN_SHORT_PRESS_MIN_MS    50
#define BTN_LONG_PRESS_MIN_MS     5000
#define BTN_SOFTWARE_DEBOUNCE_MS  250

pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  g_state_cond  = PTHREAD_COND_INITIALIZER;
system_state_t  g_system_state;

static pthread_t s_clock_tid;
static pthread_t s_weather_tid;
static pthread_t s_webserver_tid;
static pthread_t s_buzzer_tid;
static pthread_t s_smartconfig_tid;
static pthread_t s_btn_tid;

static void sigusr1_handler(int sig)
{
    (void)sig;
    /* No-op handler: used to interrupt blocking system calls (poll/read) for instant shutdown */
}

static void signal_handler(int sig)
{
    printf("\n[main] Caught signal %d. Shutting down system cleanly...\n", sig);
    pthread_mutex_lock(&g_state_mutex);
    g_system_state.running = false;
    pthread_cond_broadcast(&g_state_cond);
    pthread_mutex_unlock(&g_state_mutex);

    smartconfig_wakeup();

    /* Immediately interrupt blocking poll/read calls in all worker threads */
    if (s_clock_tid) pthread_kill(s_clock_tid, SIGUSR1);
    if (s_weather_tid) pthread_kill(s_weather_tid, SIGUSR1);
    if (s_webserver_tid) pthread_kill(s_webserver_tid, SIGUSR1);
    if (s_buzzer_tid) pthread_kill(s_buzzer_tid, SIGUSR1);
    if (s_smartconfig_tid) pthread_kill(s_smartconfig_tid, SIGUSR1);
    if (s_btn_tid) pthread_kill(s_btn_tid, SIGUSR1);
}

/* Kiểm tra xem file wpa_supplicant.conf đã lưu cấu hình mạng từ trước hay chưa */
static bool get_saved_wifi_ssid(char *out_ssid, size_t max_len)
{
    FILE *fp = fopen(WPA_CONF_FILE, "r");
    if (!fp) return false;

    char buf[256];
    bool has_network = false;
    if (out_ssid && max_len > 0) {
        out_ssid[0] = '\0';
    }

    while (fgets(buf, sizeof(buf), fp)) {
        char *p_ssid = strstr(buf, "ssid=");
        if (p_ssid) {
            has_network = true;
            if (out_ssid && max_len > 0) {
                p_ssid += 5;
                if (*p_ssid == '"') p_ssid++;
                size_t len = strcspn(p_ssid, "\"\r\n");
                if (len >= max_len) len = max_len - 1;
                strncpy(out_ssid, p_ssid, len);
                out_ssid[len] = '\0';
            }
            break;
        } else if (strstr(buf, "network={")) {
            has_network = true;
        }
    }
    fclose(fp);
    return has_network;
}

void system_state_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_destroy(&g_state_mutex);
    pthread_mutex_init(&g_state_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_mutex_lock(&g_state_mutex);
    g_system_state.current_screen = SCREEN_CLOCK;
    g_system_state.alarm_ringing = false;
    time_t now_init = time(NULL);
    struct tm tm_init;
    localtime_r(&now_init, &tm_init);
    if (alarm_manager_is_already_handled(&tm_init)) {
        g_system_state.last_triggered_minute = tm_init.tm_min;
    } else {
        g_system_state.last_triggered_minute = -1;
    }
    g_system_state.force_weather_fetch = false;
    g_system_state.running = true;

    memset(&g_system_state.weather_data, 0, sizeof(g_system_state.weather_data));
    g_system_state.weather_data.is_valid = false;

    /* Nạp cấu hình báo thức lưu trên ổ nhớ Flash */
    alarm_manager_load_config(ALARM_CONFIG_FILE, &g_system_state.alarm_config);

    /* Tự động nhận diện mạng khi khởi động */
    char saved_ssid[64] = {0};
    if (get_saved_wifi_ssid(saved_ssid, sizeof(saved_ssid))) {
        g_system_state.net_mode = MODE_STATION;
        printf("[main] Found saved Wi-Fi profile. Attempting auto-reconnect...\n");
        smartconfig_trigger_station(saved_ssid, "");
    } else {
        g_system_state.net_mode = MODE_SOFT_AP;
        printf("[main] No Wi-Fi profile found. Starting Soft AP mode...\n");
        smartconfig_trigger_softap();
    }

    pthread_mutex_unlock(&g_state_mutex);
}

void system_state_destroy(void)
{
    pthread_cond_destroy(&g_state_cond);
    pthread_mutex_destroy(&g_state_mutex);
}

/**
 * @brief Dispatch button actions based on duration and system context priority matrix
 * @param duration_ms Press duration in milliseconds
 */
static void dispatch_button_action(uint64_t duration_ms)
{
    pthread_mutex_lock(&g_state_mutex);
    net_mode_t current_net = g_system_state.net_mode;

    /* Ngữ cảnh 0: Nhấn giữ >= 5000ms -> Chuyển đổi Soft AP / Station */
    if (duration_ms >= BTN_LONG_PRESS_MIN_MS) {
        printf("[btn_thread] LONG PRESS (%llu ms) -> Toggle SmartConfig\n", (unsigned long long)duration_ms);
        if (g_system_state.alarm_ringing) {
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            alarm_manager_record_state(&tm_now, true);
        }
        g_system_state.alarm_ringing = false;
        pthread_cond_broadcast(&g_state_cond);
        pthread_mutex_unlock(&g_state_mutex);

        if (current_net == MODE_STATION) {
            smartconfig_trigger_softap();
        } else {
            smartconfig_trigger_station("", "");
        }
    }
    /* Ngữ cảnh 1 & 2: Nhấn ngắn (50ms <= t < 5000ms) */
    else if (duration_ms >= BTN_SHORT_PRESS_MIN_MS) {
        printf("[btn_thread] SHORT PRESS (%llu ms)\n", (unsigned long long)duration_ms);
        /* [P2-M9] Ưu tiên 1: Tắt còi báo thức ngay lập tức nếu đang kêu */
        if (g_system_state.alarm_ringing) {
            g_system_state.alarm_ringing = false;
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            alarm_manager_record_state(&tm_now, true);
            pthread_cond_broadcast(&g_state_cond);
            pthread_mutex_unlock(&g_state_mutex);
            printf("[btn_thread] Alarm silenced by user.\n");
        }
        /* [P2-M6] Ưu tiên 2: Chuyển màn hình CLOCK <-> WEATHER */
        else {
            if (g_system_state.current_screen == SCREEN_CLOCK) {
                g_system_state.current_screen = SCREEN_WEATHER;
                g_system_state.force_weather_fetch = true;
                printf("[btn_thread] Screen -> WEATHER\n");
            } else {
                g_system_state.current_screen = SCREEN_CLOCK;
                printf("[btn_thread] Screen -> CLOCK\n");
            }
            pthread_cond_broadcast(&g_state_cond);
            pthread_mutex_unlock(&g_state_mutex);
        }
    } else {
        pthread_mutex_unlock(&g_state_mutex);
    }
}

static void *btn_thread_func(void *arg)
{
    (void)arg;
    int btn_fd = -1;
    struct button_event ev;
    uint64_t press_timestamp_ns = 0;
    uint64_t last_release_time_ms = 0;

    printf("[btn_thread] Opening button device node: %s...\n", BTN_DEV_PATH);

    while (1) {
        pthread_mutex_lock(&g_state_mutex);
        bool is_running = g_system_state.running;
        pthread_mutex_unlock(&g_state_mutex);
        if (!is_running) return NULL;

        btn_fd = open(BTN_DEV_PATH, O_RDONLY);
        if (btn_fd >= 0) break;
        sleep(1);
    }

    printf("[btn_thread] Button event listener active.\n");

    while (1) {
        pthread_mutex_lock(&g_state_mutex);
        bool is_running = g_system_state.running;
        pthread_mutex_unlock(&g_state_mutex);
        if (!is_running) break;

        /* Poll button driver với timeout 500ms để không bị treo khi dừng chương trình */
        struct pollfd pfd;
        pfd.fd = btn_fd;
        pfd.events = POLLIN;
        int pret = poll(&pfd, 1, 500);

        if (pret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pret == 0) {
            continue; /* Timeout, kiểm tra lại cờ running */
        }

        ssize_t n = read(btn_fd, &ev, sizeof(struct button_event));
        if (n != sizeof(struct button_event)) {
            continue;
        }

        if (ev.state == BTN_STATE_PRESSED) {
            press_timestamp_ns = ev.timestamp_ns;
            continue;
        }

        if (ev.state == BTN_STATE_RELEASED && press_timestamp_ns > 0) {
            uint64_t release_timestamp_ns = ev.timestamp_ns;
            uint64_t duration_ms = (release_timestamp_ns - press_timestamp_ns) / 1000000ULL;
            press_timestamp_ns = 0;

            /* Đồng nhất nguồn thời gian debounce trực tiếp từ kernel interrupt timestamp */
            uint64_t current_release_ms = release_timestamp_ns / 1000000ULL;

            /* Lọc chống rung phần mềm (Software Debounce) */
            if ((current_release_ms - last_release_time_ms) < BTN_SOFTWARE_DEBOUNCE_MS) {
                printf("[btn_thread] Debounce: Ignored rapid click (<%dms)\n", BTN_SOFTWARE_DEBOUNCE_MS);
                continue;
            }
            last_release_time_ms = current_release_ms;

            /* Xử lý hành động nút nhấn theo ma trận ưu tiên */
            dispatch_button_action(duration_ms);
        }
    }

    if (btn_fd >= 0) close(btn_fd);
    printf("[btn_thread] Thread safely terminated.\n");
    return NULL;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("====================================================\n");
    printf("   SMART WEATHER ALARM CLOCK (Project 2 Engine)     \n");
    printf("====================================================\n");

    /* Kích hoạt interface Loopback 127.0.0.1 qua fork + execvp để fetch dữ liệu thời tiết nội bộ */
    pid_t lo_pid = fork();
    if (lo_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        int max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 1024) max_fd = 1024;
        for (int fd = 3; fd < max_fd; fd++) {
            close(fd);
        }
        char *const lo_cmd_ip[] = {"ip", "link", "set", "lo", "up", NULL};
        execvp(lo_cmd_ip[0], lo_cmd_ip);
        /* Fallback if ip command is not present */
        char *const lo_cmd_if[] = {"ifconfig", "lo", "127.0.0.1", "up", NULL};
        execvp(lo_cmd_if[0], lo_cmd_if);
        _exit(0);
    } else if (lo_pid > 0) {
        waitpid(lo_pid, NULL, 0);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGPIPE, SIG_IGN); /* Ignore SIGPIPE to avoid socket termination */

    system_state_init();

    if (ssd1306_init(I2C_DEV_PATH, 0x3C) < 0) {
        fprintf(stderr, "[main] Warning: SSD1306 OLED initialization failed at %s\n", I2C_DEV_PATH);
    } else {
        printf("[main] SSD1306 OLED initialized successfully.\n");
    }

    pthread_create(&s_smartconfig_tid, NULL, smartconfig_thread_func, NULL);
    pthread_create(&s_buzzer_tid, NULL, buzzer_thread_func, NULL);
    pthread_create(&s_weather_tid, NULL, weather_thread_func, NULL);
    pthread_create(&s_clock_tid, NULL, clock_thread_func, NULL);
    pthread_create(&s_webserver_tid, NULL, webserver_thread_func, NULL);
    pthread_create(&s_btn_tid, NULL, btn_thread_func, NULL);

    /* Chờ toàn bộ worker threads kết thúc an toàn */
    pthread_join(s_btn_tid, NULL);
    pthread_join(s_clock_tid, NULL);
    pthread_join(s_weather_tid, NULL);
    pthread_join(s_webserver_tid, NULL);
    pthread_join(s_buzzer_tid, NULL);
    pthread_join(s_smartconfig_tid, NULL);

    ssd1306_close();
    system_state_destroy();

    printf("[main] Application shutdown complete. Goodbye!\n");
    return 0;
}