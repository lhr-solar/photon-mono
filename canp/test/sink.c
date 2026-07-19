#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../canp.h"

#define SERVER_IP "3.141.38.115"
#define SERVER_I_PORT 6100
#define SERVER_O_PORT 6500

#define SOCKET_SNDBUF_BYTES (4 * 1024 * 1024)

int connectTCP(void) {
    int sock = {};
    struct sockaddr_in server = {};
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) return -1;

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    int sendbuf = SOCKET_SNDBUF_BYTES;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(SERVER_O_PORT);
    inet_pton(AF_INET, SERVER_IP, &server.sin_addr);

    if(connect(sock, (struct sockaddr*)&server, sizeof(server)) == 0) return sock;

    close(sock);
    return -1;
}

int main(){
    int sock = -1;
    while(sock < 0) sock = connectTCP();
    canpBatch_t* batch = (canpBatch_t*)malloc(sizeof(canpBatch_t));
    printf("connected\n");
    while(1){
        memset(batch, 0, sizeof(canpBatch_t));
        canpReadBatch(sock, batch);
        canpPrintBatch(batch);
    }
    free(batch);
};
