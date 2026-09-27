/**
 * @file mqtt_client.h
 * @brief MQTT Telemetry & Remote Control Client (HiveMQ Public Broker)
 * @author PHUC TAI
 */

#ifndef _MQTT_CLIENT_H_
#define _MQTT_CLIENT_H_

#include <stdbool.h>

#define MQTT_BROKER_HOST            "broker.hivemq.com"
#define MQTT_BROKER_PORT            "1883"
#define MQTT_CLIENT_ID_PREFIX       "smartclock_tai"
#define MQTT_TOPIC_TELEMETRY        "smartclock/tai/telemetry"
#define MQTT_TOPIC_COMMAND          "smartclock/tai/command"

#define MQTT_PUBLISH_INTERVAL_SEC   30

void *mqtt_thread_func(void *arg);
void  mqtt_trigger_publish(void);

#endif /* _MQTT_CLIENT_H_ */
