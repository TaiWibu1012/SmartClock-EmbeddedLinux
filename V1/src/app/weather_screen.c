/**
 * @file weather_screen.c
 * @brief [P2-M5] Weather Screen Controller & HTTP Client (Fetch on entry, 5-min refresh, 5s non-blocking timeout)
 * @author PHUC TAI
 */

#include "weather_screen.h"
#include "ssd1306_oled.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#define HTTP_BUF_SIZE 2048

/**
 * @brief Retrieve local IP address assigned to a specific network interface
 * @param ifname Network interface name (e.g., "wlan0", "lo")
 * @param out_ip Destination buffer for IP string
 * @param max_len Size of destination buffer
 * @return true if IP was successfully retrieved, false otherwise
 */
static bool get_interface_ip(const char *ifname, char *out_ip, size_t max_len)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
    }
    if (fd < 0) return false;
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        close(fd);
        return false;
    }
    close(fd);

    struct sockaddr_in *ipaddr = (struct sockaddr_in *)&ifr.ifr_addr;
    strncpy(out_ip, inet_ntoa(ipaddr->sin_addr), max_len - 1);
    out_ip[max_len - 1] = '\0';
    return true;
}

/**
 * @brief Parse JSON weather response to extract temperature and condition
 * @param json_body String containing HTTP body payload
 * @param out_data Destination struct for parsed weather data
 * @return true if both fields were parsed successfully, false otherwise
 */
static bool parse_weather_json(const char *json_body, weather_data_t *out_data)
{
    if (!json_body || !out_data) return false;

    const char *temp_pos = strstr(json_body, "\"temp\":");
    const char *cond_pos = strstr(json_body, "\"condition\":");

    if (!temp_pos || !cond_pos) return false;

    temp_pos += strlen("\"temp\":");
    while (*temp_pos == ' ' || *temp_pos == ':') temp_pos++;
    out_data->temperature = (float)atof(temp_pos);

    cond_pos += strlen("\"condition\":");
    while (*cond_pos == ' ' || *cond_pos == '\"') cond_pos++;

    int idx = 0;
    while (*cond_pos != '\"' && *cond_pos != '}' && *cond_pos != '\0' && idx < 31) {
        out_data->condition[idx++] = *cond_pos++;
    }
    out_data->condition[idx] = '\0';
    out_data->is_valid = true;

    return true;
}

/**
 * @brief Fetch weather information from Mock Weather Server via non-blocking TCP HTTP GET
 * @param out_data Destination struct for retrieved weather information
 * @return true on success, false on network error or timeout
 */
static bool fetch_http_weather_single(weather_data_t *out_data, bool *out_timed_out)
{
    int sock_fd = -1;
    struct sockaddr_in server_addr;
    struct timeval tv_timeout;
    char request[256];
    char response[HTTP_BUF_SIZE];
    bool success = false;

    if (out_timed_out) *out_timed_out = false;

    sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock_fd < 0) {
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    }
    if (sock_fd < 0) {
        perror("weather_screen: Failed to create socket");
        goto cleanup;
    }
    fcntl(sock_fd, F_SETFD, FD_CLOEXEC);

    /* Đưa socket về chế độ Non-blocking để kiểm soát chặt timeout connect() */
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    tv_timeout.tv_sec = WEATHER_TIMEOUT_SEC;
    tv_timeout.tv_usec = 0;
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_timeout, sizeof(tv_timeout));
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv_timeout, sizeof(tv_timeout));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(WEATHER_SERVER_PORT);

    if (inet_pton(AF_INET, WEATHER_SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("weather_screen: Invalid server IP");
        goto cleanup;
    }

    int res = connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (res < 0) {
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = sock_fd;
            pfd.events = POLLOUT;
            int poll_ret;

            /* Retry poll() if interrupted by system signal (EINTR) */
            do {
                poll_ret = poll(&pfd, 1, WEATHER_TIMEOUT_SEC * 1000);
            } while (poll_ret < 0 && errno == EINTR);

            if (poll_ret <= 0) {
                if (out_timed_out) *out_timed_out = true;
                printf("weather_screen: Connect timed out (%ds)\n", WEATHER_TIMEOUT_SEC);
                goto cleanup;
            }

            int sock_err = 0;
            socklen_t err_len = sizeof(sock_err);
            if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &sock_err, &err_len) < 0 || sock_err != 0) {
                printf("weather_screen: Connect error: %s\n", strerror(sock_err ? sock_err : errno));
                goto cleanup;
            }
        } else {
            perror("weather_screen: Connect failed immediately");
            goto cleanup;
        }
    }

    /* Khôi phục blocking mode với SO_RCVTIMEO cho các lệnh send/recv */
    fcntl(sock_fd, F_SETFL, flags);

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "User-Agent: SmartClock/1.0\r\n"
             "Connection: close\r\n\r\n",
             WEATHER_REQUEST_PATH, WEATHER_SERVER_IP, WEATHER_SERVER_PORT);

    if (send(sock_fd, request, strlen(request), MSG_NOSIGNAL) < 0) {
        perror("weather_screen: Send failed");
        goto cleanup;
    }

    memset(response, 0, sizeof(response));
    ssize_t bytes_read = recv(sock_fd, response, sizeof(response) - 1, 0);
    if (bytes_read > 0) {
        response[bytes_read] = '\0';
        char *body = strstr(response, "\r\n\r\n");
        if (body) {
            body += 4;
            if (parse_weather_json(body, out_data)) {
                success = true;
            }
        }
    }

cleanup:
    /* Guaranteed Resource Management: single exit cleanup point */
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
    }
    return success;
}

static bool fetch_http_weather(weather_data_t *out_data)
{
    int attempt = 0;
    while (attempt < 3) {
        attempt++;
        bool is_timeout = false;
        if (fetch_http_weather_single(out_data, &is_timeout)) {
            return true;
        }
        if (is_timeout) {
            break; /* Don't retry if 5-second connection timeout expired (TC-P2-06) */
        }

        pthread_mutex_lock(&g_state_mutex);
        bool running = g_system_state.running;
        pthread_mutex_unlock(&g_state_mutex);
        if (!running) break;

        if (attempt < 3) {
            usleep(300 * 1000); /* 300ms pause for transient startup */
        }
    }
    return false;
}

/**
 * @brief Render Weather UI to OLED Display (Called strictly by clock_thread / Master Arbitrator)
 * @param data Current weather metrics
 * @param net_mode Active network mode (Station / Soft AP)
 */
void render_weather_ui(const weather_data_t *data, net_mode_t net_mode)
{
    char temp_str[16];
    char net_str[24];
    char ip_str[24] = "No IP";
    char footer_str[32];

    ssd1306_clear();

    /* 1. Fetch IP Address */
    if (net_mode == MODE_SOFT_AP) {
        strncpy(ip_str, "192.168.4.1", sizeof(ip_str));
    } else {
        if (!get_interface_ip("wlan0", ip_str, sizeof(ip_str))) {
            strncpy(ip_str, "No IP", sizeof(ip_str));
        }
    }

    /* =========================================================================
     * 1. HEADER BAR (Y: 0 -> 10)
     * ========================================================================= */
    if (net_mode == MODE_SOFT_AP) {
        snprintf(net_str, sizeof(net_str), "SoftAP Weather");
    } else {
        snprintf(net_str, sizeof(net_str), "Station Weather");
    }
    ssd1306_draw_string(2, 0, net_str, 1);

    if (data->is_valid) {
        ssd1306_draw_string(102, 0, "[OK]", 1);
    } else {
        ssd1306_draw_string(96, 0, "[OFF]", 1);
    }

    ssd1306_draw_hline(0, 10, SSD1306_WIDTH, 1);

    /* =========================================================================
     * 2. MAIN CENTER BODY (Y: 11 -> 48)
     * ========================================================================= */
    if (data->is_valid) {
        /* Centered Temperature */
        snprintf(temp_str, sizeof(temp_str), "%.1f C", data->temperature);
        int temp_len = strlen(temp_str);
        int temp_x = (SSD1306_WIDTH - (temp_len * 12)) / 2;
        if (temp_x < 2) temp_x = 2;
        ssd1306_draw_string(temp_x, 16, temp_str, 2);

        /* Centered Condition */
        int cond_len = strlen(data->condition);
        int cond_x = (SSD1306_WIDTH - (cond_len * 6)) / 2;
        if (cond_x < 2) cond_x = 2;
        ssd1306_draw_string(cond_x, 35, data->condition, 1);
    } else {
        ssd1306_draw_string(22, 20, "NO CONNECTION", 1);
        ssd1306_draw_string(10, 34, "Check Wi-Fi Router", 1);
    }

    /* =========================================================================
     * 3. FOOTER BAR (Y: 49 -> 63) - SHORTENED URL TO PREVENT OVERFLOW
     * ========================================================================= */
    ssd1306_draw_hline(0, 49, SSD1306_WIDTH, 1);

    if (strcmp(ip_str, "No IP") != 0) {
        /* Format: "192.168.55.15:8080" (fits 128px screen) */
        snprintf(footer_str, sizeof(footer_str), "%s:%d", ip_str, DEFAULT_HTTP_PORT);
    } else {
        snprintf(footer_str, sizeof(footer_str), "Disconnected");
    }

    int footer_len = strlen(footer_str);
    int footer_x = (SSD1306_WIDTH - (footer_len * 6)) / 2;
    if (footer_x < 1) footer_x = 1;

    ssd1306_draw_string(footer_x, 53, footer_str, 1);

    ssd1306_update();
}

void *weather_thread_func(void *arg)
{
    (void)arg;
    time_t last_fetch_time = 0;
    weather_data_t fetched_data;
    memset(&fetched_data, 0, sizeof(fetched_data));

    printf("[weather_thread] Worker thread started.\n");

    while (1) {
        pthread_mutex_lock(&g_state_mutex);

        /* Wait until running is false OR we are in SCREEN_WEATHER OR force_weather_fetch is requested */
        while (g_system_state.running && 
               g_system_state.current_screen != SCREEN_WEATHER && 
               !g_system_state.force_weather_fetch) {
            pthread_cond_wait(&g_state_cond, &g_state_mutex);
        }

        if (!g_system_state.running) {
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }

        bool force_fetch = g_system_state.force_weather_fetch;
        if (force_fetch) {
            g_system_state.force_weather_fetch = false;
        }

        time_t now = time(NULL);
        bool need_fetch = false;
        if (g_system_state.current_screen == SCREEN_WEATHER) {
            if (force_fetch || (last_fetch_time == 0) || (now - last_fetch_time >= WEATHER_REFRESH_SEC)) {
                need_fetch = true;
            }
        }
        pthread_mutex_unlock(&g_state_mutex);

        if (need_fetch) {
            printf("[weather_thread] Querying Mock Weather Server...\n");
            last_fetch_time = time(NULL);

            bool ok = fetch_http_weather(&fetched_data);

            pthread_mutex_lock(&g_state_mutex);
            if (ok) {
                g_system_state.weather_data = fetched_data;
                g_system_state.weather_data.last_update_ts = (uint64_t)last_fetch_time;
                printf("[weather_thread] Data updated: %.1f C, %s\n",
                       fetched_data.temperature, fetched_data.condition);
            } else {
                g_system_state.weather_data.is_valid = false;
                printf("[weather_thread] Fetch failed. Marked as Offline.\n");
            }
            pthread_mutex_unlock(&g_state_mutex);
        }

        /* Wait for next 5-minute refresh cycle or user event (screen toggle / shutdown) */
        pthread_mutex_lock(&g_state_mutex);
        if (g_system_state.running && g_system_state.current_screen == SCREEN_WEATHER && !g_system_state.force_weather_fetch) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += WEATHER_REFRESH_SEC;
            pthread_cond_timedwait(&g_state_cond, &g_state_mutex, &ts);
        }
        pthread_mutex_unlock(&g_state_mutex);
    }

    printf("[weather_thread] Thread safely terminated.\n");
    return NULL;
}