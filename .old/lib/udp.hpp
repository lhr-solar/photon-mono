#pragma once

#include <cstdint>
#include <arpa//inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "shared.hpp"

struct Udp{
    int createConnection(uint32_t port, const char* IP);
    int closeConnection();
    int writePacket(std::array<uint8_t, 4096>& buffer);
    int readPacket(std::array<uint8_t, 4096>& buffer);
    int sock = -1;
    int fd = -1;
};

int Udp::readPacket(std::array<uint8_t, 4096>& buffer){
    return recv(sock, &buffer, 4096, 0);
};

int Udp::writePacket(std::array<uint8_t, 4096>& buffer){
    return send(sock, &buffer, 4096, 0);
}

int Udp::createConnection(uint32_t port, const char* IP){
    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        tLog("Udp socket failed");
        return -1;
    }
    int opt = 1;
    tLog("Udp Socket created");
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    struct sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);
    if(IP == NULL){
        set_nonblock(sock);
        if (bind(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0 ){
            close(sock);
            tLog("Udp bind failed");
            return -1;
        }
        return 0;
    }

    if (inet_pton(AF_INET, IP, &serverAddr.sin_addr) <= 0) {
        close(sock);
        tLog("Udp inet_pton failed");
        return -1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        close(sock);
        tLog("Udp connect failed");
        return -1;
    }
    const char * buffer = "[UDP] Subscribe \n";
    send(sock, buffer, strlen(buffer), 0);

    return 0;
};

int Udp::closeConnection(){
    return close(sock);
};
