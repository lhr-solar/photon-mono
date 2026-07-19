#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include "../lib/tcp.hpp"
#include "../lib/udp.hpp"
#include "../lib/shared.hpp"

inline std::string now(){
    using namespace std::chrono;

    auto now = system_clock::now();
    auto sec = time_point_cast<std::chrono::seconds>(now);
    auto ms  = duration_cast<milliseconds>(now - sec).count();

    std::time_t tt = system_clock::to_time_t(sec);
    std::tm tm = *std::localtime(&tt);

    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
       << '.' << std::setw(3) << std::setfill('0') << ms;

    return ss.str();
}

int main(void){
        Udp udpConnection;
        Tcp tcpConnection;
        udpConnection.createConnection(UDP_IPORT, aws_ip);
        tcpConnection.createConnection(TCP_IPORT, aws_ip, -1);

        std::array<uint8_t, 4096> uBuffer = {};
        std::array<uint8_t, 4096> tBuffer = {};

        while(1){
            std::string uMessage = "[UDP] Heartbeat " + now() + '\n';
            std::memcpy(uBuffer.data(), uMessage.data(), uMessage.size() + 1);
            std::string tMessage = "[TCP] Heartbeat " + now() + '\n';
            std::memcpy(tBuffer.data(), tMessage.data(), tMessage.size() + 1);

            udpConnection.writePacket(uBuffer);
            tcpConnection.writePacket(tBuffer);
        }
}
