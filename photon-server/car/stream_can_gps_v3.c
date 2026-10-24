#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <sys/mman.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include <net/if.h>
#include <sys/ioctl.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/videodev2.h>

#include "canp.h"

/* ============================================================
   CONFIG
   ============================================================ */

#define SERVER_IP "150.136.85.207"
#define SERVER_PORT 6100

/* H.264/RTP video destination. */
#define VIDEO_DEVICE "/dev/video2"
#define VIDEO_DEST_IP SERVER_IP
#define VIDEO_DEST_PORT 6200
#define VIDEO_WIDTH 1280
#define VIDEO_HEIGHT 720
#define VIDEO_FPS 30
#define VIDEO_MAX_RTP_PAYLOAD 1200
#define VIDEO_RTP_PAYLOAD_TYPE 96
#define VIDEO_MAX_NALS 16

/*
 * Local Linux transmit-queue priorities. Higher values are preferred.
 * These are best-effort and depend on the active qdisc/network path.
 */
#define TCP_SOCKET_PRIORITY 6
#define VIDEO_SOCKET_PRIORITY 1

/* Run video below the CAN/GPS/TCP work in the CPU scheduler. */
#define VIDEO_THREAD_NICE 10

/* Drop video frames while CAN/GPS telemetry is backed up. */
#define VIDEO_FIFO_PAUSE_DEPTH (MAX_BATCH_FRAMES * 4)

/* Fail a wedged TCP write/connect instead of blocking while UDP video
 * fills the LTE uplink (that freeze is what made Pit look like 4:32). */
#define TCP_CONNECT_TIMEOUT_MS 5000
#define TCP_SEND_TIMEOUT_MS 2000
#define TCP_USER_TIMEOUT_MS 5000

#define NTP_PORT "123"
#define NTP_TIMEOUT_SEC 3
#define NTP_UNIX_EPOCH_DELTA 2208988800ULL

static const char *NTP_SERVERS[] =
{
    "time.cloudflare.com",
    "time.google.com",
    "pool.ntp.org"
};

#define NTP_SERVER_COUNT (sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]))

#ifndef IFACE
#define IFACE "enx000a35001e53"
#endif

#define GPS_PORT 17335

#define FIFO_SIZE 16384

#define MAX_BATCH_FRAMES CANP_MAX_BATCH
#define FLUSH_INTERVAL_US 20000

#define GPS_CAN_ID_BASE  0x0C0

#define GPS_CAN_ID_LAT    (GPS_CAN_ID_BASE + 0)
#define GPS_CAN_ID_LON    (GPS_CAN_ID_BASE + 1)
#define GPS_CAN_ID_SPEED  (GPS_CAN_ID_BASE + 2)
#define GPS_CAN_ID_STAT   (GPS_CAN_ID_BASE + 3)

/* CAN ID mask for CAN0 + GPS */
#define CAN0_GPS_MASK 0x1000

/* ============================================================
   DATA TYPES
   ============================================================ */

typedef struct
{
    uint32_t can_id;
    uint8_t dlc;
    uint8_t data[8];
    uint8_t bus;   // 0 = can0, 1 = can1, 2 = GPS
    uint64_t rx_ms;
} __attribute__((packed)) can_packet_t;

typedef struct
{
    can_packet_t buf[FIFO_SIZE];
    uint32_t head;
    uint32_t tail;
    pthread_mutex_t lock;
} fifo_t;

static fifo_t fifo =
{
    .head = 0,
    .tail = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/* ============================================================
   GPS STATE
   ============================================================ */

typedef struct
{
    double lat;
    double lon;
    double speed_mph;
    uint8_t valid;
} gps_data_t;

static gps_data_t gps_data;
static pthread_mutex_t gps_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
   STATS
   ============================================================ */

static volatile uint64_t stat_can_rx = 0;
static volatile uint64_t stat_can_sent = 0;
static volatile uint64_t stat_tcp_sends = 0;
static volatile uint64_t stat_fifo_overwrites = 0;
static volatile uint64_t stat_reconnects = 0;

static volatile uint32_t stat_fifo_highwater = 0;

static volatile uint64_t stat_video_frames_sent = 0;
static volatile uint64_t stat_video_frames_dropped = 0;
static volatile uint64_t stat_video_send_errors = 0;

/*
 * The TCP telemetry path owns this state. Video may transmit only while
 * the TCP path says the network is healthy.
 */
static int video_network_ready = 0;
static pthread_mutex_t video_network_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
   TIME
   ============================================================ */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t current_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
}

/*
 * CAN ID as sent to the server. Extended IDs keep all 29 bits and carry
 * bit 31, as the DBC writes them. Only standard can0 IDs get CAN0_GPS_MASK.
 */
static uint32_t wire_can_id(canid_t raw, int bus0)
{
    if (raw & CAN_EFF_FLAG)
        return (raw & CAN_EFF_MASK) | 0x80000000u;

    uint32_t id = raw & CAN_SFF_MASK;
    return bus0 ? (id | CAN0_GPS_MASK) : id;
}

/* ============================================================
   NTP / SNTP TIME SYNCHRONIZATION
   ============================================================ */

typedef struct
{
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    int8_t precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t reference_id;
    uint32_t reference_seconds;
    uint32_t reference_fraction;
    uint32_t originate_seconds;
    uint32_t originate_fraction;
    uint32_t receive_seconds;
    uint32_t receive_fraction;
    uint32_t transmit_seconds;
    uint32_t transmit_fraction;
} __attribute__((packed)) ntp_packet_t;

static double timespec_to_double(const struct timespec *ts)
{
    return (double)ts->tv_sec + (double)ts->tv_nsec / 1000000000.0;
}

static void double_to_timespec(double value, struct timespec *ts)
{
    ts->tv_sec = (time_t)value;
    ts->tv_nsec = (long)((value - (double)ts->tv_sec) * 1000000000.0);

    if (ts->tv_nsec < 0)
    {
        ts->tv_nsec += 1000000000L;
        ts->tv_sec--;
    }
    else if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

static double ntp_timestamp_to_unix(uint32_t seconds_be, uint32_t fraction_be)
{
    uint32_t seconds = ntohl(seconds_be);
    uint32_t fraction = ntohl(fraction_be);

    return (double)((uint64_t)seconds - NTP_UNIX_EPOCH_DELTA) +
           (double)fraction / 4294967296.0;
}

static void unix_timestamp_to_ntp(
    const struct timespec *ts,
    uint32_t *seconds_be,
    uint32_t *fraction_be)
{
    uint64_t seconds = (uint64_t)ts->tv_sec + NTP_UNIX_EPOCH_DELTA;
    uint64_t fraction =
        ((uint64_t)ts->tv_nsec << 32) / 1000000000ULL;

    *seconds_be = htonl((uint32_t)seconds);
    *fraction_be = htonl((uint32_t)fraction);
}

static int sync_time_from_server(const char *server_name)
{
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *rp;
    int status;
    int synchronized = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    status = getaddrinfo(server_name, NTP_PORT, &hints, &results);
    if (status != 0)
    {
        fprintf(stderr, "[NTP] DNS lookup failed for %s: %s\n",
                server_name, gai_strerror(status));
        return -1;
    }

    for (rp = results; rp != NULL && !synchronized; rp = rp->ai_next)
    {
        int sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sock >= 0)
        {
            struct timeval timeout;
            timeout.tv_sec = NTP_TIMEOUT_SEC;
            timeout.tv_usec = 0;

            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                       &timeout, sizeof(timeout));

            if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
            {
                ntp_packet_t request;
                ntp_packet_t response;
                struct timespec t1;
                struct timespec t4;

                memset(&request, 0, sizeof(request));
                memset(&response, 0, sizeof(response));

                request.li_vn_mode = (0U << 6) | (4U << 3) | 3U;

                clock_gettime(CLOCK_REALTIME, &t1);

                uint32_t request_transmit_seconds;
                uint32_t request_transmit_fraction;

                unix_timestamp_to_ntp(
                    &t1,
                    &request_transmit_seconds,
                    &request_transmit_fraction);

                request.transmit_seconds = request_transmit_seconds;
                request.transmit_fraction = request_transmit_fraction;

                ssize_t sent = send(sock, &request, sizeof(request), 0);

                if (sent == (ssize_t)sizeof(request))
                {
                    ssize_t received = recv(sock, &response, sizeof(response), 0);
                    clock_gettime(CLOCK_REALTIME, &t4);

                    if (received == (ssize_t)sizeof(response))
                    {
                        uint8_t leap = (response.li_vn_mode >> 6) & 0x03;
                        uint8_t version = (response.li_vn_mode >> 3) & 0x07;
                        uint8_t mode = response.li_vn_mode & 0x07;

                        int valid_response =
                            leap != 3 &&
                            version >= 3 &&
                            (mode == 4 || mode == 5) &&
                            response.stratum > 0 &&
                            response.stratum <= 15;

                        int timestamp_matches =
                            response.originate_seconds == request.transmit_seconds &&
                            response.originate_fraction == request.transmit_fraction;

                        if (!valid_response)
                        {
                            fprintf(stderr, "[NTP] Invalid reply from %s\n",
                                    server_name);
                        }
                        else if (!timestamp_matches)
                        {
                            fprintf(stderr, "[NTP] Timestamp mismatch from %s\n",
                                    server_name);
                        }
                        else
                        {
                            double t1_seconds = timespec_to_double(&t1);
                            double t2_seconds = ntp_timestamp_to_unix(
                                response.receive_seconds,
                                response.receive_fraction);
                            double t3_seconds = ntp_timestamp_to_unix(
                                response.transmit_seconds,
                                response.transmit_fraction);
                            double t4_seconds = timespec_to_double(&t4);

                            double offset =
                                ((t2_seconds - t1_seconds) +
                                 (t3_seconds - t4_seconds)) / 2.0;

                            double corrected_time = t4_seconds + offset;
                            struct timespec corrected_ts;
                            double_to_timespec(corrected_time, &corrected_ts);

                            if (clock_settime(CLOCK_REALTIME, &corrected_ts) == 0)
                            {
                                printf("[NTP] Time synchronized using %s\\n",
                                       server_name);
                                synchronized = 1;
                            }
                            else
                            {
                                fprintf(stderr,
                                        "[NTP] clock_settime failed: %s "
                                        "(run as root or grant CAP_SYS_TIME)\\n",
                                        strerror(errno));
                            }
                        }
                    }
                    else if (received < 0)
                    {
                        fprintf(stderr, "[NTP] No reply from %s: %s\\n",
                                server_name, strerror(errno));
                    }
                }
            }

            close(sock);
        }
    }

    freeaddrinfo(results);
    return synchronized ? 0 : -1;
}

static int sync_time_from_ntp(void)
{
    for (size_t i = 0; i < NTP_SERVER_COUNT; i++)
    {
        printf("[NTP] Trying %s...\n", NTP_SERVERS[i]);

        if (sync_time_from_server(NTP_SERVERS[i]) == 0)
            return 0;
    }

    fprintf(stderr, "[NTP] Unable to synchronize time\n");
    return -1;
}

/* ============================================================
   FIFO
   ============================================================ */

static inline uint32_t fifo_depth_nolock(void)
{
    if (fifo.head >= fifo.tail)
        return fifo.head - fifo.tail;

    return FIFO_SIZE - fifo.tail + fifo.head;
}

static uint32_t fifo_depth(void)
{
    uint32_t depth;

    pthread_mutex_lock(&fifo.lock);
    depth = fifo_depth_nolock();
    pthread_mutex_unlock(&fifo.lock);

    return depth;
}

static void fifo_push(const can_packet_t *pkt)
{
    pthread_mutex_lock(&fifo.lock);

    uint32_t next = (fifo.head + 1) % FIFO_SIZE;

    if (next == fifo.tail)
    {
        fifo.tail = (fifo.tail + 1) % FIFO_SIZE;
        stat_fifo_overwrites++;
    }

    fifo.buf[fifo.head] = *pkt;
    fifo.head = next;

    uint32_t depth = fifo_depth_nolock();
    if (depth > stat_fifo_highwater)
        stat_fifo_highwater = depth;

    pthread_mutex_unlock(&fifo.lock);
}

static int fifo_pop_many(can_packet_t *dst, int max_count)
{
    pthread_mutex_lock(&fifo.lock);

    int count = 0;

    while (count < max_count && fifo.tail != fifo.head)
    {
        dst[count++] = fifo.buf[fifo.tail];
        fifo.tail = (fifo.tail + 1) % FIFO_SIZE;
    }

    pthread_mutex_unlock(&fifo.lock);

    return count;
}

/* ============================================================
   TCP
   ============================================================ */

static int interface_has_ipv4(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return 0;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", IFACE);

    int valid = 0;

    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0)
    {
        struct sockaddr_in *addr =
            (struct sockaddr_in *)&ifr.ifr_addr;

        uint32_t ip = ntohl(addr->sin_addr.s_addr);

        /*
         * Reject 0.0.0.0 and loopback addresses.
         */
        if (ip != 0 && (ip >> 24) != 127)
            valid = 1;
    }

    close(sock);
    return valid;
}

static int interface_has_carrier(void)
{
    char path[128];
    char value = '0';

    snprintf(path, sizeof(path),
             "/sys/class/net/%s/carrier", IFACE);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;

    int ok = fscanf(fp, " %c", &value);
    fclose(fp);

    return ok == 1 && value == '1';
}

static int default_route_exists(void)
{
    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp)
        return 0;

    char line[256];

    /*
     * Skip header.
     */
    if (!fgets(line, sizeof(line), fp))
    {
        fclose(fp);
        return 0;
    }

    while (fgets(line, sizeof(line), fp))
    {
        char iface[IFNAMSIZ];
        unsigned long destination;
        unsigned long gateway;
        unsigned int flags;

        if (sscanf(line, "%15s %lx %lx %x",
                   iface,
                   &destination,
                   &gateway,
                   &flags) == 4)
        {
            /*
             * Destination 00000000 means default route.
             * Flag 0x1 means the route is up.
             */
            if (strcmp(iface, IFACE) == 0 &&
                destination == 0 &&
                (flags & 0x1))
            {
                fclose(fp);
                return 1;
            }
        }
    }

    fclose(fp);
    return 0;
}

static void renew_dhcp(void)
{
    printf("[NET] Renewing DHCP...\n");

    system("ip route flush default");
    system("ip addr flush dev " IFACE);

    system("ip link set " IFACE " down");
    usleep(200000);

    system("ip link set " IFACE " up");

    /*
     * First wait for the physical Ethernet link.
     * This prevents DHCP attempts while the router or switch
     * is still powered off or booting.
     */
    while (!interface_has_carrier())
    {
        printf("[NET] Waiting for Ethernet carrier...\n");
        sleep(1);
    }

    printf("[NET] Ethernet carrier detected\n");

    while (1)
    {
        /*
         * -n: exit with failure if no lease is obtained
         * -q: exit immediately after obtaining a lease
         * -t 5: send up to five discovery attempts
         * -T 2: wait two seconds between attempts
         */
        int status =
            system("udhcpc -i " IFACE " -n -q -t 5 -T 2");

        if (status == -1)
        {
            printf("[NET] Failed to execute udhcpc\n");
        }
        else if (WIFEXITED(status) &&
                 WEXITSTATUS(status) == 0 &&
                 interface_has_ipv4() &&
                 default_route_exists())
        {
            printf("[NET] DHCP succeeded\n");
            return;
        }
        else
        {
            printf("[NET] DHCP failed or configuration incomplete\n");
        }

        /*
         * The router may have lost power again while DHCP
         * was running.
         */
        while (!interface_has_carrier())
        {
            printf("[NET] Ethernet carrier lost; waiting...\n");
            sleep(1);
        }

        sleep(2);
    }
}

static void tcp_fail_fast(int sock)
{
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));

    int socket_priority = TCP_SOCKET_PRIORITY;
    setsockopt(sock, SOL_SOCKET, SO_PRIORITY,
               &socket_priority, sizeof(socket_priority));

    int idle = 5;
    int intvl = 2;
    int cnt = 3;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

#ifdef TCP_USER_TIMEOUT
    int user_timeout = TCP_USER_TIMEOUT_MS;
    setsockopt(sock, IPPROTO_TCP, TCP_USER_TIMEOUT,
               &user_timeout, sizeof(user_timeout));
#endif

    struct timeval send_timeout;
    send_timeout.tv_sec = TCP_SEND_TIMEOUT_MS / 1000;
    send_timeout.tv_usec = (TCP_SEND_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               &send_timeout, sizeof(send_timeout));

    int sndbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
}

static int connect_tcp_once(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    tcp_fail_fast(sock);

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0)
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server.sin_addr);

    int rc = connect(sock, (struct sockaddr *)&server, sizeof(server));
    if (rc != 0 && errno != EINPROGRESS)
    {
        close(sock);
        return -1;
    }

    struct pollfd pfd = { .fd = sock, .events = POLLOUT };
    rc = poll(&pfd, 1, TCP_CONNECT_TIMEOUT_MS);
    if (rc <= 0)
    {
        close(sock);
        return -1;
    }

    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0)
    {
        close(sock);
        return -1;
    }

    if (flags >= 0)
        fcntl(sock, F_SETFL, flags);
    return sock;
}

static int connect_tcp(void)
{
    while (1)
    {
        int sock = connect_tcp_once();
        if (sock >= 0)
        {
            printf("[TCP] connected to %s:%d\n", SERVER_IP, SERVER_PORT);
            return sock;
        }

        stat_reconnects++;

        if (!interface_has_carrier() ||
            !interface_has_ipv4() ||
            !default_route_exists())
        {
            renew_dhcp();
        }
        else
        {
            printf("[TCP] Network is configured, but %s:%d failed\n",
                   SERVER_IP, SERVER_PORT);
            sleep(1);
        }
    }
}

/* ============================================================
   CAN THREAD
   ============================================================ */

static void *can_thread(void *arg)
{
    (void)arg;

    int s0 = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    int s1 = socket(PF_CAN, SOCK_RAW, CAN_RAW);

    struct ifreq ifr0, ifr1;
    struct sockaddr_can addr0, addr1;

    strcpy(ifr0.ifr_name, "can0");
    strcpy(ifr1.ifr_name, "can1");

    ioctl(s0, SIOCGIFINDEX, &ifr0);
    ioctl(s1, SIOCGIFINDEX, &ifr1);

    addr0.can_family = AF_CAN;
    addr0.can_ifindex = ifr0.ifr_ifindex;

    addr1.can_family = AF_CAN;
    addr1.can_ifindex = ifr1.ifr_ifindex;

    bind(s0, (struct sockaddr *)&addr0, sizeof(addr0));
    bind(s1, (struct sockaddr *)&addr1, sizeof(addr1));

    struct pollfd fds[2] =
    {
        { .fd = s0, .events = POLLIN },
        { .fd = s1, .events = POLLIN }
    };

    struct can_frame frame;

    while (1)
    {
        int poll_result = poll(fds, 2, -1);

        if (poll_result > 0)
        {
            if (fds[0].revents & POLLIN)
            {
                ssize_t bytes_read = read(s0, &frame, sizeof(frame));

                if (bytes_read > 0)
                {
                    /*
                     * Standard CAN ID 0x000 is generated locally by
                     * timer_can_thread(). Ignore it, but still process
                     * can1 during this same poll iteration.
                     */
                    int is_timer = !(frame.can_id & CAN_EFF_FLAG) &&
                                   (frame.can_id & CAN_SFF_MASK) == 0x000;

                    if (!is_timer)
                    {
                        can_packet_t pkt;
                        pkt.rx_ms = current_time_ms();
                        pkt.can_id = wire_can_id(frame.can_id, 1);
                        pkt.dlc = frame.can_dlc;
                        memcpy(pkt.data, frame.data, 8);
                        pkt.bus = 0;

                        fifo_push(&pkt);
                        stat_can_rx++;
                    }
                }
            }

            if (fds[1].revents & POLLIN)
            {
                ssize_t bytes_read = read(s1, &frame, sizeof(frame));

                if (bytes_read > 0)
                {
                    can_packet_t pkt;
                    pkt.rx_ms = current_time_ms();
                    pkt.can_id = wire_can_id(frame.can_id, 0);
                    pkt.dlc = frame.can_dlc;
                    memcpy(pkt.data, frame.data, 8);
                    pkt.bus = 1;

                    fifo_push(&pkt);
                    stat_can_rx++;
                }
            }
        }
    }

    return NULL;
}


/* ============================================================
   REALTIME 100 HZ CAN0 TIMER THREAD
   ============================================================ */

static void *timer_can_thread(void *arg)
{
    (void)arg;

    /*
     * Lock the memory already used by the real-time timer thread.
     * Do not use MCL_FUTURE here: the merged video path later creates
     * a thread and mmaps large V4L2 buffers after networking is ready.
     */
    mlockall(MCL_CURRENT);

    struct sched_param param;
    param.sched_priority = 85;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    cpu_set_t cp;
    CPU_ZERO(&cp);
    CPU_SET(0, &cp);
    pthread_setaffinity_np(pthread_self(), sizeof(cp), &cp);

    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);

    if (s < 0)
        return NULL;

    struct ifreq ifr;
    struct sockaddr_can addr;

    memset(&ifr, 0, sizeof(ifr));
    memset(&addr, 0, sizeof(addr));

    strcpy(ifr.ifr_name, "can0");

    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0)
    {
        close(s);
        return NULL;
    }

    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(s);
        return NULL;
    }

    struct can_frame f;
    memset(&f, 0, sizeof(f));

    f.can_id = 0x000;
    f.can_dlc = 2;

    uint16_t counter = 0;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (1)
    {
        next.tv_nsec += 10000000;
        if (next.tv_nsec >= 1000000000)
        {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }

        f.data[0] = counter & 0xFF;
        f.data[1] = counter >> 8;

        write(s, &f, sizeof(f));
        counter++;

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    return NULL;
}

/* ============================================================
   GPS THREAD
   ============================================================ */

static double nmea_to_decimal(const char *coord, char hemi)
{
    double value = atof(coord);
    int degrees = (int)(value / 100.0);
    double minutes = value - (degrees * 100.0);
    double decimal = degrees + minutes / 60.0;

    if (hemi == 'S' || hemi == 'W')
        decimal = -decimal;

    return decimal;
}

static void parse_rmc(char *line)
{
    char *fields[20];
    int count = 0;

    char *token = strtok(line, ",");

    while (token && count < 20)
        fields[count++] = token, token = strtok(NULL, ",");

    if (count < 8)
        return;

    pthread_mutex_lock(&gps_lock);

    if (fields[2][0] != 'A')
    {
        gps_data.valid = 0;
        pthread_mutex_unlock(&gps_lock);
        return;
    }

    gps_data.lat = nmea_to_decimal(fields[3], fields[4][0]);
    gps_data.lon = nmea_to_decimal(fields[5], fields[6][0]);
    gps_data.speed_mph = atof(fields[7]) * 1.15078;
    gps_data.valid = 1;

    pthread_mutex_unlock(&gps_lock);
}

static void *gps_thread(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0)
    {
        perror("[GPS] socket");
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(GPS_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("[GPS] bind");
        close(sock);
        return NULL;
    }

    printf("[GPS] Listening for GPS on UDP %d\n", GPS_PORT);

    while (1)
    {
        char buffer[2048];
        ssize_t len = recvfrom(
            sock,
            buffer,
            sizeof(buffer) - 1,
            0,
            NULL,
            NULL);

        if (len > 0)
        {
            buffer[len] = '\0';

            char *saveptr;
            char *line = strtok_r(buffer, "\r\n", &saveptr);

            while (line)
            {
                if (strncmp(line, "$GPRMC", 6) == 0)
                {
                    char tmp[256];
                    strncpy(tmp, line, sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = '\0';
                    parse_rmc(tmp);
                }

                line = strtok_r(NULL, "\r\n", &saveptr);
            }
        }
        else if (len < 0 && errno != EINTR)
        {
            fprintf(stderr, "[GPS] recvfrom failed: %s\n",
                    strerror(errno));
            sleep(1);
        }
    }

    close(sock);
    return NULL;
}

/* ============================================================
   GPS → CAN
   ============================================================ */

static void push_can(uint32_t id, const void *data, uint8_t len)
{
    can_packet_t pkt;

    pkt.rx_ms = current_time_ms();
    pkt.can_id = id | CAN0_GPS_MASK;
    pkt.dlc = len;
    memset(pkt.data, 0, 8);
    memcpy(pkt.data, data, len);
    pkt.bus = 2;

    fifo_push(&pkt);
}

static void *gps_can_thread(void *arg)
{
    (void)arg;

    while (1)
    {
        usleep(1000000);

        gps_data_t g;

        pthread_mutex_lock(&gps_lock);
        g = gps_data;
        pthread_mutex_unlock(&gps_lock);

        /* Latitude and longitude may be negative, so store them signed. */
        int32_t lat_i = (int32_t)(g.lat * 1e6);
        int32_t lon_i = (int32_t)(g.lon * 1e6);
        uint16_t spd_i = (uint16_t)(g.speed_mph * 100);
        uint8_t valid = g.valid;

        push_can(GPS_CAN_ID_LAT, &lat_i, 4);
        push_can(GPS_CAN_ID_LON, &lon_i, 4);
        push_can(GPS_CAN_ID_SPEED, &spd_i, 2);
        push_can(GPS_CAN_ID_STAT, &valid, 1);

        printf("[GPS] VALID=%s LAT=%.6f LON=%.6f\n",
               valid ? "YES" : "NO",
               g.lat,
               g.lon);
        fflush(stdout);
    }

    return NULL;
}


/* ============================================================
   VIDEO NETWORK GATE
   ============================================================ */

static void set_video_network_ready(int ready)
{
    pthread_mutex_lock(&video_network_lock);
    video_network_ready = ready ? 1 : 0;
    pthread_mutex_unlock(&video_network_lock);
}

static int get_video_network_ready(void)
{
    int ready;

    pthread_mutex_lock(&video_network_lock);
    ready = video_network_ready;
    pthread_mutex_unlock(&video_network_lock);

    return ready;
}

/* ============================================================
   H.264 RTP VIDEO THREAD
   ============================================================ */

typedef struct
{
    void *start;
    size_t length;
} video_buffer_t;

typedef struct
{
    uint8_t vpxcc;
    uint8_t mpt;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
} video_rtp_header_t;

static int video_find_start_code(
    uint8_t *buf,
    int offset,
    int buf_size,
    int *sc_len)
{
    for (int i = offset; i < buf_size - 3; i++)
    {
        if (buf[i] == 0 && buf[i + 1] == 0)
        {
            if (buf[i + 2] == 1)
            {
                *sc_len = 3;
                return i;
            }

            if (i + 3 < buf_size &&
                buf[i + 2] == 0 &&
                buf[i + 3] == 1)
            {
                *sc_len = 4;
                return i;
            }
        }
    }

    return -1;
}

static int video_extract_nals(
    uint8_t *buf,
    int buf_size,
    uint8_t **nals,
    int *nal_sizes,
    int max_nals)
{
    int offset = 0;
    int nal_count = 0;

    while (offset < buf_size && nal_count < max_nals)
    {
        int current_sc_len = 0;
        int next_sc_len = 0;

        int start = video_find_start_code(
            buf,
            offset,
            buf_size,
            &current_sc_len);

        if (start < 0)
            break;

        int next_start = video_find_start_code(
            buf,
            start + current_sc_len,
            buf_size,
            &next_sc_len);

        if (next_start < 0)
            next_start = buf_size;

        int nal_size = next_start - (start + current_sc_len);

        if (nal_size > 0)
        {
            nals[nal_count] = buf + start + current_sc_len;
            nal_sizes[nal_count] = nal_size;
            nal_count++;
        }

        offset = next_start;
    }

    return nal_count;
}

static void video_send_rtp_nal(
    int sock,
    struct sockaddr_in *addr,
    uint8_t *nal,
    int nal_size,
    uint16_t *seq,
    uint32_t *ts,
    uint32_t ssrc)
{
    if (nal_size <= 0)
        return;

    if (nal_size <= VIDEO_MAX_RTP_PAYLOAD)
    {
        uint8_t packet[12 + VIDEO_MAX_RTP_PAYLOAD];
        video_rtp_header_t *hdr = (video_rtp_header_t *)packet;

        hdr->vpxcc = 0x80;
        hdr->mpt = 0x80 | VIDEO_RTP_PAYLOAD_TYPE;
        hdr->seq = htons(*seq);
        hdr->ts = htonl(*ts);
        hdr->ssrc = htonl(ssrc);

        memcpy(packet + 12, nal, (size_t)nal_size);

        ssize_t sent = sendto(
            sock,
            packet,
            (size_t)(12 + nal_size),
            0,
            (struct sockaddr *)addr,
            sizeof(*addr));

        if (sent < 0)
            stat_video_send_errors++;

        (*seq)++;
    }
    else
    {
        uint8_t fu_indicator = (nal[0] & 0xE0) | 28;
        uint8_t nal_type = nal[0] & 0x1F;
        int offset = 1;

        while (offset < nal_size)
        {
            int remaining = nal_size - offset;
            int max_chunk = VIDEO_MAX_RTP_PAYLOAD - 2;
            int chunk_size = remaining > max_chunk ? max_chunk : remaining;
            int is_last = offset + chunk_size >= nal_size;

            uint8_t fu_header = nal_type;

            if (offset == 1)
                fu_header |= 0x80;

            if (is_last)
                fu_header |= 0x40;

            uint8_t packet[12 + VIDEO_MAX_RTP_PAYLOAD];
            video_rtp_header_t *hdr = (video_rtp_header_t *)packet;

            hdr->vpxcc = 0x80;
            hdr->mpt = (is_last ? 0x80 : 0x00) |
                       VIDEO_RTP_PAYLOAD_TYPE;
            hdr->seq = htons(*seq);
            hdr->ts = htonl(*ts);
            hdr->ssrc = htonl(ssrc);

            packet[12] = fu_indicator;
            packet[13] = fu_header;
            memcpy(packet + 14, nal + offset, (size_t)chunk_size);

            ssize_t sent = sendto(
                sock,
                packet,
                (size_t)(14 + chunk_size),
                0,
                (struct sockaddr *)addr,
                sizeof(*addr));

            if (sent < 0)
                stat_video_send_errors++;

            (*seq)++;
            offset += chunk_size;
        }
    }
}

static void *video_thread(void *arg)
{
    (void)arg;

    int fd = -1;
    int sock = -1;
    int streaming = 0;
    video_buffer_t *buffers = NULL;
    uint32_t buffer_count = 0;

    /* This affects only the calling Linux thread. */
    if (setpriority(PRIO_PROCESS, 0, VIDEO_THREAD_NICE) != 0)
    {
        fprintf(stderr, "[VIDEO] Unable to lower thread priority: %s\n",
                strerror(errno));
    }

    fd = open(VIDEO_DEVICE, O_RDWR);
    if (fd < 0)
    {
        fprintf(stderr, "[VIDEO] Cannot open %s: %s\n",
                VIDEO_DEVICE, strerror(errno));
        return NULL;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = VIDEO_WIDTH;
    fmt.fmt.pix.height = VIDEO_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        fprintf(stderr, "[VIDEO] VIDIOC_S_FMT failed: %s\n",
                strerror(errno));
        goto cleanup;
    }
    char fourcc[5] = { fmt.fmt.pix.pixelformat & 0xff, (fmt.fmt.pix.pixelformat >> 8) & 0xff, (fmt.fmt.pix.pixelformat >> 16) & 0xff, (fmt.fmt.pix.pixelformat >> 24) & 0xff, '\0' };
    printf("[VIDEO] Camera accepted %ux%u format=%s\n", fmt.fmt.pix.width, fmt.fmt.pix.height, fourcc);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0)
    {
        fprintf(stderr, "[VIDEO] VIDIOC_REQBUFS failed: %s\n",
                strerror(errno));
        goto cleanup;
    }

    buffer_count = req.count;
    buffers = calloc(buffer_count, sizeof(*buffers));

    if (!buffers)
    {
        fprintf(stderr, "[VIDEO] Buffer allocation failed\n");
        goto cleanup;
    }

    for (uint32_t i = 0; i < buffer_count; i++)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            fprintf(stderr, "[VIDEO] VIDIOC_QUERYBUF failed: %s\n",
                    strerror(errno));
            goto cleanup;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(
            NULL,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            buf.m.offset);

        if (buffers[i].start == MAP_FAILED)
        {
            buffers[i].start = NULL;
            fprintf(stderr, "[VIDEO] mmap failed: %s\n",
                    strerror(errno));
            goto cleanup;
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            fprintf(stderr, "[VIDEO] Initial VIDIOC_QBUF failed: %s\n",
                    strerror(errno));
            goto cleanup;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
    {
        fprintf(stderr, "[VIDEO] VIDIOC_STREAMON failed: %s\n",
                strerror(errno));
        goto cleanup;
    }

    streaming = 1;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        fprintf(stderr, "[VIDEO] UDP socket failed: %s\n",
                strerror(errno));
        goto cleanup;
    }

    int socket_priority = VIDEO_SOCKET_PRIORITY;
    setsockopt(sock, SOL_SOCKET, SO_PRIORITY,
               &socket_priority, sizeof(socket_priority));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(VIDEO_DEST_PORT);

    if (inet_pton(AF_INET, VIDEO_DEST_IP, &addr.sin_addr) != 1)
    {
        fprintf(stderr, "[VIDEO] Invalid destination IP: %s\n",
                VIDEO_DEST_IP);
        goto cleanup;
    }

    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0x12345678;

    printf("[VIDEO] Streaming H.264 RTP to %s:%d at %dx%d/%d FPS\n",
           VIDEO_DEST_IP,
           VIDEO_DEST_PORT,
           VIDEO_WIDTH,
           VIDEO_HEIGHT,
           VIDEO_FPS);

    while (1)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0)
        {
            if (errno == EINTR)
                continue;

            fprintf(stderr, "[VIDEO] VIDIOC_DQBUF failed: %s\n",
                    strerror(errno));
            break;
        }

        int send_frame =
            get_video_network_ready() &&
            fifo_depth() < VIDEO_FIFO_PAUSE_DEPTH;

        if (send_frame &&
            buf.index < buffer_count &&
            buffers[buf.index].start != NULL)
        {
            uint8_t *nals[VIDEO_MAX_NALS];
            int nal_sizes[VIDEO_MAX_NALS];

            int nal_count = video_extract_nals(
                buffers[buf.index].start,
                (int)buf.bytesused,
                nals,
                nal_sizes,
                VIDEO_MAX_NALS);

            for (int i = 0; i < nal_count; i++)
            {
                video_send_rtp_nal(
                    sock,
                    &addr,
                    nals[i],
                    nal_sizes[i],
                    &seq,
                    &ts,
                    ssrc);
            }

            stat_video_frames_sent++;
        }
        else
        {
            /* Intentional UDP frame drop to protect TCP telemetry. */
            stat_video_frames_dropped++;
        }

        ts += 90000U / VIDEO_FPS;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            fprintf(stderr, "[VIDEO] VIDIOC_QBUF failed: %s\n",
                    strerror(errno));
            break;
        }
    }

cleanup:
    if (streaming)
    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);
    }

    if (buffers)
    {
        for (uint32_t i = 0; i < buffer_count; i++)
        {
            if (buffers[i].start)
                munmap(buffers[i].start, buffers[i].length);
        }

        free(buffers);
    }

    if (sock >= 0)
        close(sock);

    if (fd >= 0)
        close(fd);

    fprintf(stderr,
            "[VIDEO] Video thread stopped; CAN/GPS telemetry remains active\n");

    return NULL;
}

/* ============================================================
   DEBUG
   ============================================================ */

static void *debug_thread(void *arg)
{
    (void)arg;

    while (1)
    {
        sleep(1);

        printf("CAN RX: %llu\n", (unsigned long long)stat_can_rx);
        printf("FIFO HIGH: %u\n", stat_fifo_highwater);
        printf("FIFO LEVEL: %u\n", fifo_depth());
        printf("OVERWRITES: %llu\n", (unsigned long long)stat_fifo_overwrites);
        printf("RECONNECTS: %llu\n", (unsigned long long)stat_reconnects);
        printf("VIDEO READY: %d\n", get_video_network_ready());
        printf("VIDEO SENT: %llu\n",
               (unsigned long long)stat_video_frames_sent);
        printf("VIDEO DROPPED: %llu\n",
               (unsigned long long)stat_video_frames_dropped);
        printf("VIDEO SEND ERRORS: %llu\n",
               (unsigned long long)stat_video_send_errors);
        printf("---------------------------\n");

        stat_can_rx = 0;
        stat_fifo_overwrites = 0;
    }

    return NULL;
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    pthread_t can_tid;
    pthread_t gps_tid;
    pthread_t gps_can_tid;
    pthread_t dbg_tid;
    pthread_t timer_can_tid;
    pthread_t video_tid;

    pthread_create(&can_tid, NULL, can_thread, NULL);
    pthread_create(&gps_tid, NULL, gps_thread, NULL);
    pthread_create(&gps_can_tid, NULL, gps_can_thread, NULL);
    pthread_create(&dbg_tid, NULL, debug_thread, NULL);
    pthread_create(&timer_can_tid, NULL, timer_can_thread, NULL);

    /*
     * stream_can_gps_v3 remains the sole owner of DHCP and TCP recovery.
     * The video thread is not started until this initial TCP connection
     * proves that the network path is working.
     */
    int sock = connect_tcp();

    /*
     * Synchronize CLOCK_REALTIME before telemetry timestamps are created.
     * Video remains off during this startup network work so TCP/NTP get
     * first use of the link.
     */
    while (sync_time_from_ntp() != 0)
    {
        sleep(2);
    }

    set_video_network_ready(1);

    if (pthread_create(&video_tid, NULL, video_thread, NULL) != 0)
    {
        fprintf(stderr,
                "[VIDEO] Could not create video thread; "
                "CAN/GPS telemetry will continue\n");
    }

    can_packet_t raw[MAX_BATCH_FRAMES];
    canpBatch_t batch;

    uint64_t last_flush = now_us();
    uint32_t seq = 0;

    while (1)
    {
        uint32_t depth = fifo_depth();
        uint64_t now = now_us();

        int batch_ready =
            depth > 0 &&
            (depth >= MAX_BATCH_FRAMES ||
             (now - last_flush) >= FLUSH_INTERVAL_US);

        if (!batch_ready)
        {
            usleep(100);
        }
        else
        {
            int count = fifo_pop_many(raw, MAX_BATCH_FRAMES);

            if (count > 0)
            {
                batch.seq = seq++;
                batch.timestamp = current_time_ms();
                batch.count = count;

                for (int i = 0; i < count; i++)
                {
                    uint32_t id = raw[i].can_id;
                    uint16_t dt[8] = {0};
                    uint64_t age = batch.timestamp > raw[i].rx_ms ? batch.timestamp - raw[i].rx_ms : 0;
                    dt[0] = age > 0xFFFF ? 0xFFFF : (uint16_t)age;   // clamp at ~65 s

                    batch.packets[i] = canpMakePacket(
                        id,
                        raw[i].dlc,
                        raw[i].data,
                        dt);
                }

                /*
                 * Pause UDP before a TCP write that might block. Otherwise
                 * 720p RTP fills the modem, write() never returns, and the
                 * server sees CAN die while video keeps flowing.
                 */
                if (fifo_depth() >= VIDEO_FIFO_PAUSE_DEPTH)
                    set_video_network_ready(0);

                if (canpWriteBatch(sock, &batch) <= 0)
                {
                    set_video_network_ready(0);
                    close(sock);
                    sock = connect_tcp();
                    set_video_network_ready(1);
                }
                else
                {
                    stat_can_sent += count;
                    stat_tcp_sends++;
                    last_flush = now;
                    set_video_network_ready(fifo_depth() < VIDEO_FIFO_PAUSE_DEPTH);
                }
            }
        }
    }

    return 0;
}
