#include <arpa/inet.h>
#include <math.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "canp.h"

#define DEFAULT_SERVER_IP "127.0.0.1"
#define SERVER_I_PORT 6100
#define SERVER_O_PORT 6500

#define SOCKET_SNDBUF_BYTES (4 * 1024 * 1024)

uint64_t currentTimeMillis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int connectTCP(const char *server_ip) {
    int sock = -1;
    struct sockaddr_in server = {};
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) return -1;

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    int sendbuf = SOCKET_SNDBUF_BYTES;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(SERVER_I_PORT);
    inet_pton(AF_INET, server_ip, &server.sin_addr);

    if(connect(sock, (struct sockaddr*)&server, sizeof(server)) == 0) return sock;

    close(sock);
    return -1;
}

int main(int argc, char **argv){
    const char *server_ip = DEFAULT_SERVER_IP;
    if(argc > 1) {
        server_ip = argv[1];
    } else {
        const char *env_ip = getenv("PHOTON_SERVER_IP");
        if(env_ip && strlen(env_ip) > 0) {
            server_ip = env_ip;
        }
    }

    printf("Connecting CANP source to %s:%d...\n", server_ip, SERVER_I_PORT);
    int sock = -1;
    while(sock < 0) {
        sock = connectTCP(server_ip);
        if(sock < 0) usleep(100 * 1000);
    }
    printf("connected\n");
    int seq = 0;
    canpBatch_t batch;
    uint8_t d1[8] = {0x01, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x12, 0x34};
    uint8_t d2[8] = {};
    uint16_t δt[8] = {};
    batch.count = 2;
    while(1){
        d1[0] = (!d1[0]);
        batch.packets[0] = canpMakePacket(0x7ff, 8, d1, δt);

        float x = sin(seq / M_PI);
        float y = cos(seq / M_PI);
        memcpy(d2, &x, sizeof(x));
        memcpy(d2+4, &y, sizeof(y));
        batch.packets[1] = canpMakePacket(0x7fe, 8, d2, δt);

        batch.seq = seq;
        batch.timestamp = currentTimeMillis();
        canpWriteBatch(sock, &batch);
        seq++;
        usleep(10*1000);
    }
}
