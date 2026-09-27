/**
 * @file webserver.c
 * @brief [P2-M7] Embedded HTTP Web Server for Wi-Fi & Alarm Configuration (Port 8080)
 * @author PHUC TAI
 */

#include "webserver.h"
#include "alarm_manager.h"
#include "smartconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>

/* Embedded HTML Templates */
static const char *HTML_SOFTAP_FORM =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>SmartClock Wi-Fi Setup</title><style>"
    "body{font-family:Arial,sans-serif;margin:30px;background:#f4f4f9;}"
    ".card{max-width:400px;margin:auto;padding:20px;background:#fff;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
    "input[type=text],input[type=password]{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;}"
    "button{width:100%;background:#007bff;color:#fff;padding:10px;border:none;border-radius:4px;cursor:pointer;font-size:16px;}"
    "</style></head><body><div class='card'>"
    "<h2>SmartClock Wi-Fi Setup</h2>"
    "<form method='POST' action='/wifi-config'>"
    "<label>Wi-Fi SSID:</label><input type='text' name='ssid' required>"
    "<label>Password:</label><input type='password' name='password'>"
    "<button type='submit'>Connect</button>"
    "</form></div></body></html>";

static const char *HTML_STATION_FORM =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>SmartClock Alarm Setup</title><style>"
    "body{font-family:Arial,sans-serif;margin:30px;background:#f4f4f9;}"
    ".card{max-width:400px;margin:auto;padding:20px;background:#fff;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
    "input[type=number]{width:80px;padding:8px;margin:8px 0;}"
    "button{width:100%;background:#28a745;color:#fff;padding:10px;border:none;border-radius:4px;cursor:pointer;font-size:16px;}"
    "</style></head><body><div class='card'>"
    "<h2>SmartClock Alarm Setup</h2>"
    "<form method='POST' action='/alarm-config'>"
    "<label>Hour (0-23):</label> <input type='number' name='hour' min='0' max='23' value='%02d' required><br>"
    "<label>Minute (0-59):</label> <input type='number' name='minute' min='0' max='59' value='%02d' required><br>"
    "<label><input type='checkbox' name='enabled' value='1' %s> Enable Alarm</label><br><br>"
    "<button type='submit'>Save Alarm</button>"
    "</form></div></body></html>";

static const char *HTML_SUCCESS_RESPONSE =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Configuration Saved</title><style>"
    "body{font-family:Arial,sans-serif;text-align:center;margin-top:50px;background:#f4f4f9;}"
    ".card{max-width:400px;margin:auto;padding:20px;background:#fff;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
    "</style></head><body><div class='card'>"
    "<h2>%s</h2><p>%s</p><a href='/'>Back to Home</a></div></body></html>";

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t d_idx = 0;
    while (*src && d_idx + 1 < dst_size) {
        if (*src == '+') {
            dst[d_idx++] = ' ';
            src++;
        } else if (*src == '%') {
            /* Ensure there are at least two characters after % before attempting to parse */
            if (src[1] != '\0' && src[2] != '\0') {
                int h1 = hex_to_int(src[1]);
                int h2 = hex_to_int(src[2]);
                if (h1 >= 0 && h2 >= 0) {
                    dst[d_idx++] = (char)((h1 << 4) | h2);
                    src += 3;
                } else {
                    /* Invalid hex escape sequence, copy literally */
                    dst[d_idx++] = *src++;
                }
            } else {
                /* Incomplete % escape sequence at string end, copy literally */
                dst[d_idx++] = *src++;
            }
        } else {
            dst[d_idx++] = *src++;
        }
    }
    dst[d_idx] = '\0';
}

static bool get_form_param(const char *body, const char *key, char *out_val, size_t out_size)
{
    if (!body || !key || !out_val) return false;

    char search_key[128];
    snprintf(search_key, sizeof(search_key), "%s=", key);
    const char *pos = strstr(body, search_key);

    if (!pos) {
        if (strncmp(body, search_key, strlen(search_key)) == 0) {
            pos = body;
        } else {
            return false;
        }
    }

    pos += strlen(search_key);
    char raw_buf[256];
    size_t idx = 0;

    while (*pos && *pos != '&' && *pos != ' ' && *pos != '\r' && *pos != '\n' && idx < sizeof(raw_buf) - 1) {
        raw_buf[idx++] = *pos++;
    }
    raw_buf[idx] = '\0';

    url_decode(out_val, raw_buf, out_size);
    return true;
}

static void send_http_response(int client_fd, int status_code, const char *status_text, const char *body)
{
    char header[512];
    size_t body_len = strlen(body);

    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n",
             status_code, status_text, body_len);

    if (send(client_fd, header, strlen(header), 0) < 0) {
        perror("webserver: send header failed");
        return;
    }
    if (send(client_fd, body, body_len, 0) < 0) {
        perror("webserver: send body failed");
        return;
    }
}

static void handle_client_request(int client_fd)
{
    char request_buf[HTTP_REQ_MAX_SIZE];
    memset(request_buf, 0, sizeof(request_buf));

    ssize_t bytes_read = read(client_fd, request_buf, sizeof(request_buf) - 1);
    if (bytes_read <= 0) return;

    char method[16], path[128];
    if (sscanf(request_buf, "%15s %127s", method, path) != 2) {
        send_http_response(client_fd, 400, "Bad Request", "<h1>400 Bad Request</h1>");
        return;
    }

    char *body = strstr(request_buf, "\r\n\r\n");
    if (body) {
        body += 4;
        /* Nếu là POST, kiểm tra Content-Length và gom đủ dữ liệu body nếu bị phân mảnh TCP */
        if (strcmp(method, "POST") == 0) {
            char *cl_pos = strstr(request_buf, "Content-Length:");
            if (!cl_pos) cl_pos = strstr(request_buf, "content-length:");
            if (cl_pos) {
                int content_len = atoi(cl_pos + 15);
                size_t header_len = (size_t)(body - request_buf);
                size_t cur_body_len = (size_t)bytes_read - header_len;
                while (cur_body_len < (size_t)content_len && bytes_read < (ssize_t)(sizeof(request_buf) - 1)) {
                    ssize_t n = read(client_fd, request_buf + bytes_read, sizeof(request_buf) - 1 - bytes_read);
                    if (n <= 0) break;
                    bytes_read += n;
                    request_buf[bytes_read] = '\0';
                    cur_body_len += n;
                }
            }
        }
    }

/* 1. Route: GET /weather (Mock Weather Endpoint) */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/weather") == 0) {
        const char *weather_json = "{\"temp\": 29.5, \"condition\": \"Partly Cloudy\"}\r\n";
        char full_response[512];

        snprintf(full_response, sizeof(full_response),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n"
                 "%s",
                 strlen(weather_json), weather_json);

        if (send(client_fd, full_response, strlen(full_response), 0) < 0) {
            perror("webserver: send weather json failed");
        }
        return;
    }

    pthread_mutex_lock(&g_state_mutex);
    net_mode_t cur_net = g_system_state.net_mode;
    alarm_config_t cur_alarm = g_system_state.alarm_config;
    pthread_mutex_unlock(&g_state_mutex);

    /* 2. Route: GET / */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        char response_html[HTTP_RESP_MAX_SIZE];
        if (cur_net == MODE_SOFT_AP) {
            send_http_response(client_fd, 200, "OK", HTML_SOFTAP_FORM);
        } else {
            snprintf(response_html, sizeof(response_html), HTML_STATION_FORM,
                     cur_alarm.hour, cur_alarm.minute,
                     cur_alarm.enabled ? "checked" : "");
            send_http_response(client_fd, 200, "OK", response_html);
        }
    } 
    /* 3. Route: POST /wifi-config */
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/wifi-config") == 0) {
        char ssid[64] = {0}, password[64] = {0};
        if (body && get_form_param(body, "ssid", ssid, sizeof(ssid))) {
            get_form_param(body, "password", password, sizeof(password));
            printf("[webserver] Received Wi-Fi credentials: SSID='%s'\n", ssid);
            smartconfig_trigger_station(ssid, password);

            char response_html[HTTP_RESP_MAX_SIZE];
            snprintf(response_html, sizeof(response_html), HTML_SUCCESS_RESPONSE,
                     "Connecting to Wi-Fi...",
                     "Device is connecting to network. Please check the OLED display.");
            send_http_response(client_fd, 200, "OK", response_html);
        } else {
            send_http_response(client_fd, 400, "Bad Request", "<h1>Missing SSID</h1>");
        }
    } 
    /* 4. Route: POST /alarm-config */
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/alarm-config") == 0) {
        char hour_str[16] = {0}, minute_str[16] = {0}, enabled_str[16] = {0};
        if (body && get_form_param(body, "hour", hour_str, sizeof(hour_str)) &&
            get_form_param(body, "minute", minute_str, sizeof(minute_str))) {
            int hour = atoi(hour_str);
            int minute = atoi(minute_str);
            bool enabled = get_form_param(body, "enabled", enabled_str, sizeof(enabled_str));

            alarm_config_t new_cfg;
            new_cfg.hour = (hour >= 0 && hour <= 23) ? hour : DEFAULT_ALARM_HOUR;
            new_cfg.minute = (minute >= 0 && minute <= 59) ? minute : DEFAULT_ALARM_MINUTE;
            new_cfg.enabled = enabled;

            alarm_manager_save_config_atomic(ALARM_CONFIG_FILE, &new_cfg);

            pthread_mutex_lock(&g_state_mutex);
            g_system_state.alarm_config = new_cfg;
            g_system_state.last_triggered_minute = -1;
            if (!new_cfg.enabled && g_system_state.alarm_ringing) {
                g_system_state.alarm_ringing = false;
                pthread_cond_broadcast(&g_state_cond);
            }
            alarm_manager_clear_state();
            pthread_mutex_unlock(&g_state_mutex);

            printf("[webserver] Updated Alarm: %02d:%02d (Enabled: %d)\n",
                   new_cfg.hour, new_cfg.minute, new_cfg.enabled);

            char response_html[HTTP_RESP_MAX_SIZE];
            snprintf(response_html, sizeof(response_html), HTML_SUCCESS_RESPONSE,
                     "Alarm Saved Successfully",
                     "New alarm schedule has been persisted to storage.");
            send_http_response(client_fd, 200, "OK", response_html);
        } else {
            send_http_response(client_fd, 400, "Bad Request", "<h1>Invalid Alarm Data</h1>");
        }
    } 
    /* 5. 404 Not Found */
    else {
        send_http_response(client_fd, 404, "Not Found", "<h1>404 Page Not Found</h1>");
    }
}

void *webserver_thread_func(void *arg)
{
    (void)arg;
    int server_fd = -1;
    struct sockaddr_in server_addr;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
    }
    if (server_fd < 0) {
        perror("webserver: Failed to create socket");
        return NULL;
    }

    fcntl(server_fd, F_SETFD, FD_CLOEXEC);

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("webserver: setsockopt SO_REUSEADDR failed");
    }
#ifdef SO_REUSEPORT
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        /* SO_REUSEPORT optional */
    }
#endif

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(WEBSERVER_PORT);

    /* Resilient Bind Loop: Retry for up to 5s if port is temporarily held during quick restart */
    int bind_retries = 0;
    while (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        pthread_mutex_lock(&g_state_mutex);
        bool is_running = g_system_state.running;
        pthread_mutex_unlock(&g_state_mutex);

        if (!is_running || ++bind_retries > 15) {
            perror("webserver: Bind failed");
            close(server_fd);
            return NULL;
        }
        usleep(300 * 1000);
    }

    if (listen(server_fd, 5) < 0) {
        perror("webserver: Listen failed");
        close(server_fd);
        return NULL;
    }

    printf("[webserver] Embedded Webserver listening on port %d...\n", WEBSERVER_PORT);

    /* Main Poll & Accept Loop */
    while (1) {
        pthread_mutex_lock(&g_state_mutex);
        bool is_running = g_system_state.running;
        pthread_mutex_unlock(&g_state_mutex);

        if (!is_running) {
            break;
        }

        /* Use poll() with 500ms timeout to avoid hanging when exit signal arrives */
        struct pollfd pfd;
        pfd.fd = server_fd;
        pfd.events = POLLIN;
        int poll_ret = poll(&pfd, 1, 500);

        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (poll_ret == 0) {
            continue;
        }

        if (pfd.revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

            if (client_fd < 0) {
                if (errno != EINTR) {
                    perror("webserver: accept failed");
                }
                continue;
            }

            fcntl(client_fd, F_SETFD, FD_CLOEXEC);

            /*
             * Resource Management: client_fd is guaranteed to be closed
             * via the cleanup label below, regardless of which code path
             * handle_client_request() takes (early return, error, etc.).
             */
            struct timeval tv_timeout;
            tv_timeout.tv_sec = CLIENT_TIMEOUT_SEC;
            tv_timeout.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_timeout, sizeof(tv_timeout));

            handle_client_request(client_fd);

            /* Guaranteed cleanup: close client FD on all exit paths */
            close(client_fd);
        }
    }

    if (server_fd >= 0) {
        close(server_fd);
    }

    printf("[webserver] Thread safely terminated.\n");
    return NULL;
}