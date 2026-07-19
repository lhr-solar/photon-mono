#pragma once
#include <fcntl.h>
#include <ios>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <thread>

#define TCP_IPORT 6100
#define UDP_IPORT 6200

#define OBS_IPORT 5300
#define OBS_OPORT 5400

#define TCP_OPORT 6500
#define UDP_OPORT 6600

#define CORSA_FIPORT 6300
#define CORSA_FOPORT 6400

#define aws_ip "3.141.38.115"
#define local_ip "127.0.0.1"

void inline tLog(std::string s){
    const auto id = std::this_thread::get_id();
    std::cout << "[0x" << std::hex << id << "] " << s << std::endl;
};

struct UdpClient{
    uint32_t ip{};
    uint32_t port{};
    bool operator ==(const UdpClient& o) const {
        return ip == o.ip && port == o.port;
    }
};

struct UdpClientState{
    sockaddr_in addr{};
    uint64_t time{};
};

struct UdpClientHash{
    size_t operator()(const UdpClient& u) const{
        return (static_cast<size_t>(u.ip) << 16) ^ static_cast<size_t>(u.port);
    }
};

static inline int set_nonblock(int fd){
    int f = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, f | O_NONBLOCK); 
}

struct tcpSourceMetaData {
    uint64_t totalPacketsReceived{};
    uint64_t bytesTxThisConnection{};
    uint64_t totalBytesTransmitted{};
    uint64_t totalConnections{};
    uint64_t totalDisconnects{};
    uint64_t receiveFailures{};
    uint64_t lastPacketBytes{};
    uint64_t timeSinceLastConnection{};
    uint64_t timeSinceLastDisconnect{};
    uint64_t timeSinceLastReceive{};
    uint16_t listenPort{};
    bool     listenerActive{};
    bool     sourceCurrentlyConnected{};
    bool     sourceEverConnected{};
    bool     sourceDataStale{};
};
struct udpSourceMetaData {
    uint64_t totalPacketsReceived{};
    uint64_t bytesTxThisConnection{};
    uint64_t totalBytesTransmitted{};
    uint64_t totalConnections{};
    uint64_t totalDisconnects{};
    uint64_t receiveFailures{};
    uint64_t lastPacketBytes{};
    uint64_t timeSinceLastConnection{};
    uint64_t timeSinceLastDisconnect{};
    uint64_t timeSinceLastReceive{};
    uint16_t listenPort{};
    bool     listenerActive{};
    bool     sourceCurrentlyConnected{};
    bool     sourceEverConnected{};
    bool     sourceDataStale{};
};
struct tcpClientMetaData {
    uint64_t totalBytesTransmitted{};
    uint64_t totalPacketsTransmitted{};
    uint64_t totalDispatchCycles{};
    uint64_t idleDispatchCycles{};
    uint64_t clientsCurrentlyConnected{};
    uint64_t totalClientConnections{};
    uint64_t totalClientDisconnects{};
    uint64_t peakClientConnections{};
    uint64_t transmissionFailures{};
    uint64_t partialTransmissions{};
    uint64_t lastWriteBytes{};
    uint64_t lastWriteFanout{};
    uint64_t timeSinceLastConnection{};
    uint64_t timeSinceLastDisconnect{};
    uint64_t timeSinceLastTransmission{};
    uint16_t listenPort{};
    bool     listenerActive{};
};
struct udpClientMetaData {
    uint64_t totalBytesTransmitted{};
    uint64_t totalPacketsTransmitted{};
    uint64_t totalDispatchCycles{};
    uint64_t idleDispatchCycles{};
    uint64_t clientsCurrentlyConnected{};
    uint64_t totalClientConnections{};
    uint64_t totalClientDisconnects{};
    uint64_t peakClientConnections{};
    uint64_t transmissionFailures{};
    uint64_t lastWriteBytes{};
    uint64_t lastWriteFanout{};
    uint64_t subscribeMessages{};
    uint64_t unsubscribeMessages{};
    uint64_t errorQueueDisconnects{};
    uint64_t timeSinceLastConnection{};
    uint64_t timeSinceLastDisconnect{};
    uint64_t timeSinceLastTransmission{};
    uint16_t listenPort{};
    bool     listenerActive{};
    bool     requiresSubscription{};
};
struct serverMetaData {
    uint64_t uptime{};
    uint64_t observerFramesPublished{};
    uint64_t observerClientsCurrentlyConnected{};
    uint64_t observerTotalConnections{};
    uint64_t observerTotalDisconnects{};
    uint64_t observerPeakConnections{};
    uint64_t observerTransmissionFailures{};
    uint64_t timeSinceLastObserverConnection{};
    uint64_t timeSinceLastObserverDisconnect{};
    uint64_t timeSinceLastObserverPublish{};
    uint16_t observerPort{};
    bool     observerListenerActive{};
};
struct metaData {
    struct serverMetaData serverMetaData{};
    struct tcpSourceMetaData tcpSourceMetaData{};
    struct udpSourceMetaData udpSourceMetaData{};
    struct tcpClientMetaData tcpClientMetaData{};
    struct udpClientMetaData udpClientMetaData{};
};
