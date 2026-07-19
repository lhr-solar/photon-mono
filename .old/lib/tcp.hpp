#pragma once

#include <cstdint>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>
#include <fcntl.h>

#include "shared.hpp"

struct Tcp{
    int createConnection(uint32_t port, const char* IP, int wait);
    int closeConnection();
    int writePacket(std::array<uint8_t, 4096>& buffer);
    int readPacket(std::array<uint8_t, 4096>& buffer);
    int sock = -1;
    int client_fd = -1;
};

static inline void set_tcp_low_latency(int fd){
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

int Tcp::writePacket(std::array<uint8_t, 4096>& buffer){
    int fd = (client_fd >= 0) ? client_fd : sock;
    return send(fd, &buffer, 4096, 0);
}

int Tcp::readPacket(std::array<uint8_t, 4096>& buffer){
    int fd = (client_fd >= 0) ? client_fd : sock;
    return recv(fd, &buffer, 4096, 0);
};

int Tcp::createConnection(uint32_t port, const char* IP, int wait){
    tLog("Tcp Socket created");
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0){
        tLog("Tcp socket failed");
        return -1;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if(IP == NULL){ // server
        if(bind(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0){
            close(sock);
            tLog("Tcp bind failed");
            return -1;
        }
        if(listen(sock, 64) < 0){
            close(sock);
            tLog("Tcp listen failed");
            return -1;
        }
        if(wait != -1){
            client_fd = accept(sock, NULL, NULL);
            if(client_fd < 0){
                close(sock);
                tLog("Tcp accept failed");
                return -1;
            }
            set_tcp_low_latency(client_fd);
            if(set_nonblock(client_fd) < 0){
                close(client_fd);
                close(sock);
                client_fd = -1;
                tLog("Tcp accepted socket nonblock failed");
                return -1;
            }
            return 0;
        } else {
            int flags = fcntl(sock, F_GETFL, 0);
            if (flags < 0) return -1;
            if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
            return 0;
        }
    }

    if(inet_pton(AF_INET, IP, &serverAddr.sin_addr) <= 0){
        close(sock);
        tLog("Tcp inet_pton failed");
        return -1;
    }
    while(connect(sock, reinterpret_cast<sockaddr*>(&serverAddr),sizeof(serverAddr)) < 0){
        tLog("Tcp connect failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    set_tcp_low_latency(sock);
    return 0;
};

int Tcp::closeConnection(){
    int rc = 0;
    if(client_fd >= 0){
        rc = close(client_fd);
        client_fd = -1;
    }
    if(sock >= 0){
        rc = close(sock);
        sock = -1;
    }
    return rc;
};
