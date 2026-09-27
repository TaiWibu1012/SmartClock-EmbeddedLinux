/**
 * @file mqtt_client.c
 * @brief MQTT Telemetry & Remote Control Client implementation using MQTT-C
 * @author PHUC TAI
 */

#include "mqtt_client.h"
#include "system_state.h"
#include "alarm_manager.h"
#include "ssd1306_oled.h"
#include "mqtt/mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>

static bool s_trigger_publish_flag = false;

void mqtt_trigger_publish(void)
{
    s_trigger_publish_flag = true;
}

/**
 * @brief Non-blocking TCP socket connector with 5-second timeout
 */
static int open_mqtt_socket(const char *addr, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *servinfo = NULL;
    struct addrinfo *p = NULL;
    int sockfd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;

    int rv = getaddrinfo(addr, port, &hints, &servinfo);
    if (rv != 0) {
        printf("[mqtt] getaddrinfo failed for %s:%s: %s\n", addr, port, gai_strerror(rv));
        return -1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) continue;

        /* Set non-blocking for connection timeout */
        int flags = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

        int res = connect(sockfd, p->ai_addr, p->ai_addrlen);
        if (res < 0) {
            if (errno == EINPROGRESS) {
                struct pollfd pfd;
                pfd.fd = sockfd;
                pfd.events = POLLOUT;
                int pret = poll(&pfd, 1, 5000); /* 5-second timeout */
                if (pret > 0) {
                    int sock_err = 0;
                    socklen_t err_len = sizeof(sock_err);
                    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sock_err, &err_len) == 0 && sock_err == 0) {
                        /* Connection established successfully */
                        break;
                    }
                }
            }
            close(sockfd);
            sockfd = -1;
            continue;
        }
        break;
    }

    freeaddrinfo(servinfo);
    return sockfd;
}

static const char *json_get_val(const char *json, const char *key)
{
    const char *p = strstr(json, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\"')) p++;
    return p;
}

/**
 * @brief Callback invoked when a message arrives on subscribed topics
 */
static void on_mqtt_message_received(void **unused, struct mqtt_response_publish *published)
{
    (void)unused;
    char topic[128] = {0};
    char payload[512] = {0};

    size_t topic_len = published->topic_name_size;
    if (topic_len >= sizeof(topic)) topic_len = sizeof(topic) - 1;
    memcpy(topic, published->topic_name, topic_len);
    topic[topic_len] = '\0';

    size_t payload_len = published->application_message_size;
    if (payload_len >= sizeof(payload)) payload_len = sizeof(payload) - 1;
    memcpy(payload, published->application_message, payload_len);
    payload[payload_len] = '\0';

    printf("[mqtt] Received message on topic '%s': %s\n", topic, payload);

    /* Command Dispatcher */

    /* 1. Toggle / Switch Screen Command */
    if (strstr(payload, "toggle_screen") != NULL ||
        strstr(payload, "SWITCH_SCREEN") != NULL ||
        strstr(payload, "\"action\":\"toggle\"") != NULL ||
        strstr(payload, "\"action\": \"toggle\"") != NULL ||
        strstr(payload, "\"action\":\"screen\"") != NULL ||
        strstr(payload, "\"action\": \"screen\"") != NULL) {

        pthread_mutex_lock(&g_state_mutex);
        if (strstr(payload, "WEATHER") != NULL || strstr(payload, "weather") != NULL) {
            g_system_state.current_screen = SCREEN_WEATHER;
            g_system_state.force_weather_fetch = true;
            printf("[mqtt] Action: Screen explicitly set to WEATHER\n");
        } else if (strstr(payload, "CLOCK") != NULL || strstr(payload, "clock") != NULL) {
            g_system_state.current_screen = SCREEN_CLOCK;
            printf("[mqtt] Action: Screen explicitly set to CLOCK\n");
        } else {
            /* Toggle between CLOCK and WEATHER */
            g_system_state.current_screen = (g_system_state.current_screen == SCREEN_CLOCK) ? SCREEN_WEATHER : SCREEN_CLOCK;
            if (g_system_state.current_screen == SCREEN_WEATHER) {
                g_system_state.force_weather_fetch = true;
            }
            printf("[mqtt] Action: Screen toggled to %s\n",
                   (g_system_state.current_screen == SCREEN_CLOCK) ? "CLOCK" : "WEATHER");
        }
        ssd1306_force_full_update();
        pthread_cond_broadcast(&g_state_cond);
        pthread_mutex_unlock(&g_state_mutex);
        mqtt_trigger_publish();
    }
    /* 2. Silence Alarm Command */
    else if (strstr(payload, "silence") != NULL ||
             strstr(payload, "SILENCE") != NULL ||
             strstr(payload, "SILENCE_ALARM") != NULL) {

        pthread_mutex_lock(&g_state_mutex);
        if (g_system_state.alarm_ringing) {
            g_system_state.alarm_ringing = false;
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            alarm_manager_record_state(&tm_now, true);
            pthread_cond_broadcast(&g_state_cond);
            printf("[mqtt] Action: Alarm silenced remotely via MQTT command.\n");
        } else {
            printf("[mqtt] Action: Silence received but alarm is not ringing.\n");
        }
        pthread_mutex_unlock(&g_state_mutex);
        mqtt_trigger_publish();
    }
    /* 3. Set Alarm Command */
    else if (strstr(payload, "set_alarm") != NULL ||
             strstr(payload, "SET_ALARM") != NULL) {

        const char *p_h = json_get_val(payload, "\"hour\"");
        const char *p_m = json_get_val(payload, "\"minute\"");
        const char *p_e = json_get_val(payload, "\"enabled\"");

        if (p_h && p_m) {
            int h = atoi(p_h);
            int m = atoi(p_m);
            bool en = true;
            if (p_e) {
                en = (strncmp(p_e, "false", 5) != 0);
            }

            if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
                pthread_mutex_lock(&g_state_mutex);
                g_system_state.alarm_config.hour = h;
                g_system_state.alarm_config.minute = m;
                g_system_state.alarm_config.enabled = en;
                alarm_manager_save_config_atomic(ALARM_CONFIG_FILE, &g_system_state.alarm_config);
                pthread_cond_broadcast(&g_state_cond);
                pthread_mutex_unlock(&g_state_mutex);
                printf("[mqtt] Action: Alarm updated remotely to %02d:%02d (%s)\n",
                       h, m, en ? "ENABLED" : "DISABLED");
            } else {
                printf("[mqtt] Action: Invalid alarm time %02d:%02d\n", h, m);
            }
        } else {
            printf("[mqtt] Action: Missing hour or minute in set_alarm payload\n");
        }
        mqtt_trigger_publish();
    }
    /* 4. Fetch Weather Command */
    else if (strstr(payload, "fetch_weather") != NULL ||
             strstr(payload, "FETCH_WEATHER") != NULL) {

        pthread_mutex_lock(&g_state_mutex);
        g_system_state.force_weather_fetch = true;
        pthread_cond_broadcast(&g_state_cond);
        pthread_mutex_unlock(&g_state_mutex);
        printf("[mqtt] Action: Immediate weather fetch triggered.\n");
    }
    else {
        printf("[mqtt] Warning: Unknown command payload: %s\n", payload);
    }
}


/**
 * @brief Build and publish JSON system telemetry to MQTT broker
 */
static void publish_telemetry(struct mqtt_client *client, uint64_t uptime_sec)
{
    char json_buf[512];
    pthread_mutex_lock(&g_state_mutex);
    net_mode_t mode = g_system_state.net_mode;
    screen_mode_t screen = g_system_state.current_screen;
    float temp = g_system_state.weather_data.temperature;
    int hum = g_system_state.weather_data.humidity;
    const char *cond = g_system_state.weather_data.condition;
    bool alarm_en = g_system_state.alarm_config.enabled;
    int alarm_h = g_system_state.alarm_config.hour;
    int alarm_m = g_system_state.alarm_config.minute;
    bool alarm_ring = g_system_state.alarm_ringing;
    pthread_mutex_unlock(&g_state_mutex);

    snprintf(json_buf, sizeof(json_buf),
             "{"
             "\"uptime\":%llu,"
             "\"net_mode\":\"%s\","
             "\"screen\":\"%s\","
             "\"temp\":%.1f,"
             "\"humidity\":%d,"
             "\"condition\":\"%s\","
             "\"alarm\":{\"enabled\":%s,\"time\":\"%02d:%02d\",\"ringing\":%s}"
             "}",
             (unsigned long long)uptime_sec,
             (mode == MODE_STATION) ? "STATION" : ((mode == MODE_SOFT_AP) ? "SOFT_AP" : "TRANSITIONING"),
             (screen == SCREEN_CLOCK) ? "CLOCK" : "WEATHER",
             temp, hum, cond[0] ? cond : "Unknown",
             alarm_en ? "true" : "false", alarm_h, alarm_m,
             alarm_ring ? "true" : "false");

    mqtt_publish(client, MQTT_TOPIC_TELEMETRY, json_buf, strlen(json_buf), MQTT_PUBLISH_QOS_0);
    printf("[mqtt] Published telemetry (%zu bytes): %s\n", strlen(json_buf), json_buf);
}

/**
 * @brief Background Worker Thread managing MQTT connection and sync
 */
void *mqtt_thread_func(void *arg)
{
    (void)arg;
    struct mqtt_client client;
    uint8_t sendbuf[2048];
    uint8_t recvbuf[1024];
    int sockfd = -1;
    time_t last_pub_time = 0;
    time_t start_time = time(NULL);

    printf("[mqtt] Worker thread started. Target broker: %s:%s\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);

    while (1) {
        pthread_mutex_lock(&g_state_mutex);
        bool running = g_system_state.running;
        net_mode_t net_mode = g_system_state.net_mode;
        pthread_mutex_unlock(&g_state_mutex);
        if (!running) break;

        /* Only connect when in Station mode (Wi-Fi connected to internet) */
        if (net_mode != MODE_STATION) {
            sleep(2);
            continue;
        }

        /* 1. Connect to Broker */
        sockfd = open_mqtt_socket(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
        if (sockfd < 0) {
            printf("[mqtt] Unable to connect to %s:%s. Retrying in 10s...\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
            sleep(10);
            continue;
        }

        /* 2. Initialize MQTT-C client instance */
        mqtt_init(&client, sockfd, sendbuf, sizeof(sendbuf), recvbuf, sizeof(recvbuf), on_mqtt_message_received);

        char client_id[64];
        snprintf(client_id, sizeof(client_id), "%s_%ld", MQTT_CLIENT_ID_PREFIX, (long)time(NULL) % 10000);
        uint8_t connect_flags = MQTT_CONNECT_CLEAN_SESSION;
        mqtt_connect(&client, client_id, NULL, NULL, 0, NULL, NULL, connect_flags, 60);

        if (client.error != MQTT_OK) {
            printf("[mqtt] MQTT connect error: %s\n", mqtt_error_str(client.error));
            close(sockfd);
            sockfd = -1;
            sleep(5);
            continue;
        }

        printf("[mqtt] Connected to HiveMQ! Subscribing to '%s'...\n", MQTT_TOPIC_COMMAND);
        mqtt_subscribe(&client, MQTT_TOPIC_COMMAND, 0);

        /* Initial telemetry publish */
        publish_telemetry(&client, (uint64_t)(time(NULL) - start_time));
        last_pub_time = time(NULL);

        /* 3. Event Sync Loop */
        while (1) {
            pthread_mutex_lock(&g_state_mutex);
            bool is_running = g_system_state.running;
            net_mode_t cur_mode = g_system_state.net_mode;
            pthread_mutex_unlock(&g_state_mutex);

            if (!is_running || cur_mode != MODE_STATION) break;

            /* Sync incoming and outgoing MQTT packets */
            mqtt_sync(&client);

            if (client.error != MQTT_OK) {
                printf("[mqtt] Connection lost: %s. Reconnecting...\n", mqtt_error_str(client.error));
                break;
            }

            time_t now = time(NULL);
            if ((now - last_pub_time) >= MQTT_PUBLISH_INTERVAL_SEC || s_trigger_publish_flag) {
                s_trigger_publish_flag = false;
                last_pub_time = now;
                publish_telemetry(&client, (uint64_t)(now - start_time));
            }

            usleep(100 * 1000); /* 100ms sync interval */
        }

        /* Graceful cleanup before reconnect or exit */
        if (client.error == MQTT_OK) {
            mqtt_disconnect(&client);
        }
        if (sockfd >= 0) {
            close(sockfd);
            sockfd = -1;
        }
    }

    printf("[mqtt] Thread safely terminated.\n");
    return NULL;
}
