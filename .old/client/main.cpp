#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <chrono>
#include <poll.h>
#include <thread>
#include "../lib/tcp.hpp"
#include "../lib/udp.hpp"
#include "../lib/shared.hpp"

int main(void){
    Tcp tcpConnection;
    Udp udpConnection;
    static constexpr const char* UDP_SUBSCRIBE = "[UDP] Subscribe \n";
    static constexpr const char* UDP_UNSUBSCRIBE = "[UDP] Unsubscribe \n";

    tcpConnection.createConnection(TCP_OPORT, aws_ip, 0);
    udpConnection.createConnection(UDP_OPORT, aws_ip);

    while(1){
        std::array<uint8_t, 4096> tcpBuffer = {};
        std::array<uint8_t, 4096> udpBuffer = {};
        int n = {};

        n = tcpConnection.readPacket(tcpBuffer);
        std::cout.write(reinterpret_cast<const char*>(tcpBuffer.data()), n);
        std::cout << std::flush;

//        n = udpConnection.readPacket(udpBuffer);
//        std::cout.write(reinterpret_cast<const char*>(udpBuffer.data()), n);
//        std::cout << std::flush;
    }
    send(udpConnection.sock, UDP_UNSUBSCRIBE, std::strlen(UDP_UNSUBSCRIBE), 0);
}
