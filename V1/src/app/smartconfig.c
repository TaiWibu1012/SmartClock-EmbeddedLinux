/**
 * @file smartconfig.c
 * @brief [P2-M1] [P2-M3] Wi-Fi Configuration Manager (Soft AP / Station Mode & SNTP NTP Sync UTC+7)
 * @author PHUC TAI
 */

#include "smartconfig.h"
#include "../../include/smartclock_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/wait.h>
#include <fcntl.h>

#define NTP_SERVER              "pool.ntp.org"
#define NTP_FALLBACK_IP         "216.239.35.0" /* time.google.com */
#define NTP_PORT                123
#define NTP_PACKET_SIZE         48
#define NTP_TIMESTAMP_DELTA     2208988800ull /* Seconds between 1900 and 1970 */
#define VIETNAM_TIMEZONE_OFFSET (7 * 3600)    /* UTC+7 in seconds (25200s) */

static net_request_t s_pending_request = REQ_NET_NONE;
static wifi_creds_t  s_pending_creds;
static pthread_mutex_t s_net_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_net_cond  = PTHREAD_COND_INITIALIZER;

/**
 * @brief Execute command directly via fork() + execvp() and waitpid() without shell overhead or zombies
 * @param argv NULL-terminated argument array
 * @return 0 on success, non-zero exit status or -1 on failure
 */
static int safe_exec(char *const argv[])
{
    if (!argv || !argv[0]) return -1;

    printf("[smartconfig:exec] %s\n", argv[0]);

    pid_t pid = fork();
    if (pid < 0) {
        perror("[smartconfig] fork failed");
        return -1;
    }

    if (pid == 0) {
        /* Redirect stderr to /dev/null to silence background noise */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        /* Close all inherited file descriptors (>= 3) so background daemons
         * (e.g. wpa_supplicant -B, udhcpd) never inherit sockets (port 8080) or device nodes */
        int max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 1024) max_fd = 1024;
        for (int fd = 3; fd < max_fd; fd++) {
            close(fd);
        }

        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("[smartconfig] waitpid failed");
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return -1;
    } else {
        return -1;
    }
}

/* Helper to get dynamic IP of wlan0 interface directly via Linux ioctl */
static bool get_wlan0_ip(char *out_ip, size_t max_len)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

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

/* Built-in Lightweight SNTP Client (Pure C Socket UDP 123) */
static bool sync_ntp_time(const char *server_host)
{
    int sockfd = -1;
    uint8_t packet[NTP_PACKET_SIZE];
    struct sockaddr_in serv_addr;
    struct timeval timeout = {3, 0}; /* 3 seconds timeout */

    printf("[smartconfig] Querying NTP Server: %s...\n", server_host);

    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        perror("[smartconfig] Failed to create UDP socket for NTP");
        return false;
    }

    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(NTP_PORT);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", NTP_PORT);

    if (getaddrinfo(server_host, port_str, &hints, &res) != 0 || !res) {
        printf("[smartconfig] NTP DNS lookup failed. Falling back to IP %s...\n", NTP_FALLBACK_IP);
        serv_addr.sin_addr.s_addr = inet_addr(NTP_FALLBACK_IP);
    } else {
        memcpy(&serv_addr, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);
    }

    /* Construct standard NTP Request Packet (LI = 0, VN = 3, Mode = 3 Client) */
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x1B; 

    if (sendto(sockfd, packet, NTP_PACKET_SIZE, 0, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[smartconfig] Failed to send NTP packet");
        close(sockfd);
        return false;
    }

    socklen_t addr_len = sizeof(serv_addr);
    if (recvfrom(sockfd, packet, NTP_PACKET_SIZE, 0, (struct sockaddr *)&serv_addr, &addr_len) < 0) {
        printf("[smartconfig] NTP sync timeout / No response.\n");
        close(sockfd);
        return false;
    }
    close(sockfd);

    /* Extract Transmit Timestamp (Bytes 40-43) */
    uint32_t secs_since_1900 = (packet[40] << 24) | (packet[41] << 16) | (packet[42] << 8) | packet[43];
    if (secs_since_1900 < NTP_TIMESTAMP_DELTA) {
        printf("[smartconfig] Invalid NTP timestamp received.\n");
        return false;
    }

    uint64_t raw_epoch = (uint64_t)(secs_since_1900 - NTP_TIMESTAMP_DELTA);

    /* Year 2038 / Integer Overflow Safe Check */
    if (raw_epoch > (uint64_t)(INT64_MAX - VIETNAM_TIMEZONE_OFFSET)) {
        printf("[smartconfig] NTP timestamp overflow error.\n");
        return false;
    }

    /* Apply UTC+7 Offset for Vietnam Timezone safely */
    time_t epoch_time = (time_t)(raw_epoch + VIETNAM_TIMEZONE_OFFSET);

    /* Set Linux Kernel Wall-Clock Time */
    struct timespec ts;
    ts.tv_sec = epoch_time;
    ts.tv_nsec = 0;
    if (clock_settime(CLOCK_REALTIME, &ts) == 0) {
        printf("[smartconfig] System time successfully synced to Vietnam Time (UTC+7)! Epoch: %ld\n", (long)epoch_time);
        return true;
    }

    perror("[smartconfig] Failed to set system clock via clock_settime");
    return false;
}

static bool write_wpa_supplicant_conf(const char *ssid, const char *password)
{
    char *const cmd_mkdir[] = {"mkdir", "-p", "/etc/wpa_supplicant", NULL};
    safe_exec(cmd_mkdir);

    FILE *fp = fopen(WPA_CONF_FILE, "w");
    if (!fp) {
        perror("smartconfig: Failed to open wpa_supplicant.conf");
        return false;
    }

    fprintf(fp,
            "ctrl_interface=/var/run/wpa_supplicant\n"
            "update_config=1\n\n"
            "network={\n"
            "    ssid=\"%s\"\n"
            "    psk=\"%s\"\n"
            "    key_mgmt=WPA-PSK\n"
            "}\n",
            ssid, password);

    fflush(fp);
    fclose(fp);
    return true;
}

static bool is_wifi_connected(void)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        /* Close all inherited file descriptors (>= 3) except STDOUT */
        int max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 1024) max_fd = 1024;
        for (int fd = 3; fd < max_fd; fd++) {
            close(fd);
        }

        char *const args[] = {"wpa_cli", "-i", "wlan0", "status", NULL};
        execvp(args[0], args);
        _exit(127);
    }

    close(pipefd[1]);
    char buf[512];
    ssize_t n;
    bool connected = false;
    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        if (strstr(buf, "wpa_state=COMPLETED")) {
            connected = true;
            break;
        }
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return connected;
}

static void execute_start_softap(void)
{
    printf("[smartconfig] Transitioning to Soft AP mode via wpa_supplicant...\n");

    /* 1. Đảm bảo thư mục tồn tại */
    char *const cmd_mkdir[] = {"mkdir", "-p", "/etc/wpa_supplicant", "/var/lib/misc", NULL};
    safe_exec(cmd_mkdir);

    /* 2. Đảm bảo file cấu hình Soft AP tồn tại */
    FILE *f_ap = fopen("/etc/wpa_supplicant/softap.conf", "r");
    if (!f_ap) {
        f_ap = fopen("/etc/wpa_supplicant/softap.conf", "w");
        if (f_ap) {
            fprintf(f_ap,
                    "ctrl_interface=/var/run/wpa_supplicant\n"
                    "ap_scan=1\n\n"
                    "network={\n"
                    "    ssid=\"%s\"\n"
                    "    mode=2\n"
                    "    key_mgmt=WPA-PSK\n"
                    "    psk=\"12345678\"\n"
                    "    frequency=2437\n"
                    "}\n",
                    SOFTAP_DEFAULT_SSID);
            fclose(f_ap);
        }
    } else {
        fclose(f_ap);
    }

    /* 3. Đảm bảo file cấu hình DHCP server tồn tại */
    FILE *f_dhcp = fopen("/etc/udhcpd.conf", "r");
    if (!f_dhcp) {
        f_dhcp = fopen("/etc/udhcpd.conf", "w");
        if (f_dhcp) {
            fprintf(f_dhcp,
                    "start %s\n"
                    "end %s\n"
                    "interface wlan0\n"
                    "opt subnet %s\n"
                    "opt router %s\n",
                    SOFTAP_DHCP_START_IP, SOFTAP_DHCP_END_IP, SOFTAP_NETMASK, SOFTAP_GATEWAY_IP);
            fclose(f_dhcp);
        }
    } else {
        fclose(f_dhcp);
    }

    /* 4. Dừng dịch vụ cũ và gán IP tĩnh */
    char *const cmd_rfkill[] = {"rfkill", "unblock", "wifi", NULL};
    safe_exec(cmd_rfkill);

    char *const cmd_kill1[] = {"killall", "-9", "wpa_supplicant", "udhcpc", "udhcpd", NULL};
    safe_exec(cmd_kill1);

    char *const cmd_ifdown[] = {"ifconfig", "wlan0", "down", NULL};
    safe_exec(cmd_ifdown);

    char *const cmd_ifup_ip[] = {"ifconfig", "wlan0", SOFTAP_GATEWAY_IP, "netmask", SOFTAP_NETMASK, "up", NULL};
    safe_exec(cmd_ifup_ip);

    /* 5. Khởi chạy AP bằng wpa_supplicant và cấp IP bằng udhcpd */
    char *const cmd_wpa_ap[] = {"wpa_supplicant", "-B", "-i", "wlan0", "-c", "/etc/wpa_supplicant/softap.conf", NULL};
    int ret_wpa = safe_exec(cmd_wpa_ap);
    if (ret_wpa != 0) {
        printf("[smartconfig] WARNING: wpa_supplicant AP start failed (exit=%d)\n", ret_wpa);
    }

    char *const cmd_touch[] = {"touch", "/var/lib/misc/udhcpd.leases", NULL};
    safe_exec(cmd_touch);

    char *const cmd_udhcpd[] = {"udhcpd", "/etc/udhcpd.conf", NULL};
    int ret_dhcp = safe_exec(cmd_udhcpd);
    if (ret_dhcp != 0) {
        printf("[smartconfig] WARNING: udhcpd start failed (exit=%d)\n", ret_dhcp);
    }

    pthread_mutex_lock(&g_state_mutex);
    g_system_state.net_mode = MODE_SOFT_AP;
    pthread_mutex_unlock(&g_state_mutex);

    printf("\n========================================================\n");
    printf("  [smartconfig] SOFT AP ACTIVE : SmartClock_Setup\n");
    printf("  [smartconfig] WEB SETUP LINK : http://192.168.4.1:8080\n");
    printf("========================================================\n\n");
}

static void execute_connect_station(const char *ssid, const char *password)
{
    printf("[smartconfig] Connecting to Station SSID: %s...\n", ssid);

    /* Dừng Soft AP */
    char *const cmd_kill_ap[] = {"killall", "-9", "wpa_supplicant", "udhcpd", NULL};
    safe_exec(cmd_kill_ap);

    if (strlen(ssid) > 0 && strlen(password) > 0) {
        write_wpa_supplicant_conf(ssid, password);
    }

    char *const cmd_kill_st[] = {"killall", "-9", "wpa_supplicant", "udhcpc", NULL};
    safe_exec(cmd_kill_st);

    char *const cmd_ifup[] = {"ifconfig", "wlan0", "up", NULL};
    safe_exec(cmd_ifup);

    char *const cmd_wpa_st[] = {"wpa_supplicant", "-B", "-i", "wlan0", "-c", "/etc/wpa_supplicant/wpa_supplicant.conf", NULL};
    int ret_wpa_st = safe_exec(cmd_wpa_st);
    if (ret_wpa_st != 0) {
        printf("[smartconfig] WARNING: wpa_supplicant station start failed (exit=%d)\n", ret_wpa_st);
    }

    bool connected = false;
    int elapsed = 0;

    while (elapsed < WIFI_CONNECT_TIMEOUT_S) {
        sleep(WIFI_CHECK_INTERVAL_S);
        elapsed += WIFI_CHECK_INTERVAL_S;

        if (is_wifi_connected()) {
            connected = true;
            break;
        }
        printf("[smartconfig] Checking Wi-Fi status (%d/%ds)...\n", elapsed, WIFI_CONNECT_TIMEOUT_S);
    }

    if (connected) {
        printf("[smartconfig] Wi-Fi Connected! Requesting IP via DHCP...\n");
        char *const cmd_udhcpc[] = {"udhcpc", "-i", "wlan0", "-n", "-q", NULL};
        safe_exec(cmd_udhcpc);

        /* Lấy và in địa chỉ IP cấp từ Router ra Terminal Log */
        char assigned_ip[32] = {0};
        if (get_wlan0_ip(assigned_ip, sizeof(assigned_ip))) {
            printf("\n========================================================\n");
            printf("  [smartconfig] IP ASSIGNED    : %s\n", assigned_ip);
            printf("  [smartconfig] WEB SETUP LINK : http://%s:8080\n", assigned_ip);
            printf("========================================================\n\n");
        } else {
            printf("[smartconfig] Connected, but waiting for IP lease...\n");
        }

        /* Đồng bộ giờ chuẩn NTP (UTC+7) qua cả SNTP client và systemd-timesyncd daemon */
        sync_ntp_time(NTP_SERVER);
        char *const cmd_timesyncd[] = {"systemctl", "restart", "systemd-timesyncd", NULL};
        safe_exec(cmd_timesyncd);

        pthread_mutex_lock(&g_state_mutex);
        g_system_state.net_mode = MODE_STATION;
        pthread_mutex_unlock(&g_state_mutex);
    } else {
        printf("[smartconfig] Connection failed. Falling back to Soft AP...\n");
        execute_start_softap();
    }
}

void smartconfig_trigger_softap(void)
{
    pthread_mutex_lock(&s_net_mutex);
    s_pending_request = REQ_NET_START_SOFTAP;
    pthread_cond_signal(&s_net_cond);
    pthread_mutex_unlock(&s_net_mutex);
}

void smartconfig_trigger_station(const char *ssid, const char *password)
{
    pthread_mutex_lock(&s_net_mutex);
    s_pending_request = REQ_NET_CONNECT_STATION;
    strncpy(s_pending_creds.ssid, ssid, sizeof(s_pending_creds.ssid) - 1);
    s_pending_creds.ssid[sizeof(s_pending_creds.ssid) - 1] = '\0';
    strncpy(s_pending_creds.password, password, sizeof(s_pending_creds.password) - 1);
    s_pending_creds.password[sizeof(s_pending_creds.password) - 1] = '\0';
    pthread_cond_signal(&s_net_cond);
    pthread_mutex_unlock(&s_net_mutex);
}

void smartconfig_wakeup(void)
{
    pthread_mutex_lock(&s_net_mutex);
    pthread_cond_broadcast(&s_net_cond);
    pthread_mutex_unlock(&s_net_mutex);
}

void *smartconfig_thread_func(void *arg)
{
    (void)arg;
    printf("[smartconfig] Worker thread started.\n");

    while (1) {
        net_request_t req = REQ_NET_NONE;
        wifi_creds_t creds;

        pthread_mutex_lock(&s_net_mutex);
        while (s_pending_request == REQ_NET_NONE) {
            pthread_mutex_lock(&g_state_mutex);
            bool is_running = g_system_state.running;
            pthread_mutex_unlock(&g_state_mutex);
            if (!is_running) break;

            pthread_cond_wait(&s_net_cond, &s_net_mutex);
        }

        pthread_mutex_lock(&g_state_mutex);
        bool is_running = g_system_state.running;
        pthread_mutex_unlock(&g_state_mutex);

        if (!is_running && s_pending_request == REQ_NET_NONE) {
            pthread_mutex_unlock(&s_net_mutex);
            break;
        }

        req = s_pending_request;
        creds = s_pending_creds;
        s_pending_request = REQ_NET_NONE;
        pthread_mutex_unlock(&s_net_mutex);

        if (req == REQ_NET_START_SOFTAP) {
            execute_start_softap();
        } else if (req == REQ_NET_CONNECT_STATION) {
            execute_connect_station(creds.ssid, creds.password);
        }
    }

    printf("[smartconfig] Thread safely terminated.\n");
    return NULL;
}