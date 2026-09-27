/**
 * @file webserver.h
 * @brief Lightweight Embedded HTTP Webserver on port 8080 for configuration
 * @author PHUC TAI
 */

#ifndef _WEBSERVER_H_
#define _WEBSERVER_H_

#include "system_state.h"
#include "../../include/smartclock_common.h"

#define WEBSERVER_PORT          DEFAULT_HTTP_PORT
#define CLIENT_TIMEOUT_SEC      5
#define HTTP_REQ_MAX_SIZE       4096
#define HTTP_RESP_MAX_SIZE      4096

/**
 * @brief Main worker thread for the embedded configuration webserver
 * @param arg Unused
 * @return NULL on thread termination
 */
void *webserver_thread_func(void *arg);

#endif /* _WEBSERVER_H_ */