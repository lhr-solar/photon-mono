// server
#include <asm-generic/socket.h>
#include <assert.h>
#include <array>
#include <ios>
#include <ostream>
#include <ratio>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <chrono>
#include <stdio.h> 
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errqueue.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <functional>

#include "../lib/shared.hpp"
#include "../lib/spmc.hpp"
#include "../lib/tcp.hpp"
#include "../lib/udp.hpp"
#include "../lib/epoll.hpp"

static constexpr const char* UDP_SUBSCRIBE = "[UDP] Subscribe";
static constexpr const char* UDP_UNSUBSCRIBE = "[UDP] Unsubscribe";
static constexpr uint64_t SOURCE_STALE_MS = 1500;

struct TcpPacket{
    std::array<uint8_t, 4096> data{};
    uint32_t len{};
};

static inline bool starts_with(const uint8_t* buf, ssize_t len, const char* text){
    size_t tlen = std::strlen(text);
    return len >= static_cast<ssize_t>(tlen) && std::memcmp(buf, text, tlen) == 0;
}

static inline uint64_t now_ms(){
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static inline uint64_t elapsed_ms(uint64_t timestamp, uint64_t now){
    return (timestamp == 0 || now < timestamp) ? 0 : now - timestamp;
}

static inline void refresh_source_metadata(
    tcpSourceMetaData& meta,
    uint64_t now,
    uint64_t last_connection,
    uint64_t last_disconnect,
    uint64_t last_receive
){
    meta.timeSinceLastConnection = elapsed_ms(last_connection, now);
    meta.timeSinceLastDisconnect = elapsed_ms(last_disconnect, now);
    meta.timeSinceLastReceive = elapsed_ms(last_receive, now);
    meta.sourceDataStale =
        meta.sourceCurrentlyConnected &&
        last_receive != 0 &&
        meta.timeSinceLastReceive > SOURCE_STALE_MS;
}

static inline void refresh_source_metadata(
    udpSourceMetaData& meta,
    uint64_t now,
    uint64_t last_connection,
    uint64_t last_disconnect,
    uint64_t last_receive
){
    meta.timeSinceLastConnection = elapsed_ms(last_connection, now);
    meta.timeSinceLastDisconnect = elapsed_ms(last_disconnect, now);
    meta.timeSinceLastReceive = elapsed_ms(last_receive, now);
    meta.sourceDataStale =
        meta.sourceCurrentlyConnected &&
        last_receive != 0 &&
        meta.timeSinceLastReceive > SOURCE_STALE_MS;
}

static inline void refresh_client_metadata(
    tcpClientMetaData& meta,
    uint64_t now,
    uint64_t last_connection,
    uint64_t last_disconnect,
    uint64_t last_transmission
){
    meta.timeSinceLastConnection = elapsed_ms(last_connection, now);
    meta.timeSinceLastDisconnect = elapsed_ms(last_disconnect, now);
    meta.timeSinceLastTransmission = elapsed_ms(last_transmission, now);
}

static inline void refresh_client_metadata(
    udpClientMetaData& meta,
    uint64_t now,
    uint64_t last_connection,
    uint64_t last_disconnect,
    uint64_t last_transmission
){
    meta.timeSinceLastConnection = elapsed_ms(last_connection, now);
    meta.timeSinceLastDisconnect = elapsed_ms(last_disconnect, now);
    meta.timeSinceLastTransmission = elapsed_ms(last_transmission, now);
}

static void drain_udp_errqueue(
    int fd,
    std::unordered_map<UdpClient, UdpClientState, UdpClientHash>& clients,
    uint64_t* disconnects = nullptr
){
    while(true){
        sockaddr_in offender{};
        iovec iov{};
        uint8_t data[1];
        iov.iov_base = data;
        iov.iov_len = sizeof(data);

        uint8_t cbuf[512];
        msghdr msg{};
        msg.msg_name = &offender;
        msg.msg_namelen = sizeof(offender);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);

        ssize_t n = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }

        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
             cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_IP || cmsg->cmsg_type != IP_RECVERR) continue;
            auto* err = reinterpret_cast<sock_extended_err*>(CMSG_DATA(cmsg));
            if (err == nullptr) continue;
            if (err->ee_origin != SO_EE_ORIGIN_ICMP) continue;
            if (err->ee_type != ICMP_DEST_UNREACH) continue;
            if (err->ee_code != ICMP_PORT_UNREACH) continue;

            sockaddr* sa = SO_EE_OFFENDER(err);
            if (sa != nullptr && sa->sa_family == AF_INET) {
                auto* sin = reinterpret_cast<sockaddr_in*>(sa);
                UdpClient key{sin->sin_addr.s_addr, sin->sin_port};
                if(clients.erase(key) > 0 && disconnects != nullptr){
                    ++(*disconnects);
                }
            }
        }
    }
}

void udpSource(struct Udp& udpInstance, 
               std::function<void(const std::array<uint8_t, 4096>&)> writeFn,
               std::function<void(const struct udpSourceMetaData&)> writeMetaData){
    udpSourceMetaData m{};
    m.listenPort = UDP_IPORT;
    uint64_t lastConnectionTs = 0;
    uint64_t lastDisconnectTs = 0;
    uint64_t lastReceiveTs = 0;
    while(1){
        tLog("Udp Source is running");
        m.listenerActive = false;
        refresh_source_metadata(m, now_ms(), lastConnectionTs, lastDisconnectTs, lastReceiveTs);
        writeMetaData(m);
        if (udpInstance.createConnection(UDP_IPORT, NULL) < 0) {
            tLog("Udp createConnection failed");
            ++m.receiveFailures;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        tLog("Created Udp Connection");
        m.listenerActive = true;
        std::array<uint8_t, 4096> ReadBuffer = {};
        while(1){
            int err = udpInstance.readPacket(ReadBuffer);
            if (err > 0) {
                uint64_t now = now_ms();
                if(!m.sourceCurrentlyConnected){
                    m.sourceCurrentlyConnected = true;
                    m.sourceEverConnected = true;
                    m.bytesTxThisConnection = 0;
                    ++m.totalConnections;
                    lastConnectionTs = now;
                }
                ++m.totalPacketsReceived;
                m.bytesTxThisConnection += static_cast<uint64_t>(err);
                m.totalBytesTransmitted += static_cast<uint64_t>(err);
                m.lastPacketBytes = static_cast<uint64_t>(err);
                lastReceiveTs = now;
                refresh_source_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastReceiveTs);
                writeFn(ReadBuffer);
                writeMetaData(m);
                continue;
            }
            if (err == 0) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                refresh_source_metadata(m, now_ms(), lastConnectionTs, lastDisconnectTs, lastReceiveTs);
                writeMetaData(m);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            tLog("Udp Connection Lost");
            uint64_t now = now_ms();
            ++m.receiveFailures;
            if(m.sourceCurrentlyConnected){
                m.sourceCurrentlyConnected = false;
                ++m.totalDisconnects;
                lastDisconnectTs = now;
            }
            refresh_source_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastReceiveTs);
            writeMetaData(m);
            break;
        }
        udpInstance.closeConnection();
        m.listenerActive = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
};

void tcpSource(struct Tcp& tcpInstance, 
               std::function<void(const TcpPacket&)> writeFn,
               std::function<void(const struct tcpSourceMetaData&)> writeMetaData){
    tcpSourceMetaData m{};
    m.listenPort = TCP_IPORT;
    uint64_t lastConnectionTs = 0;
    uint64_t lastDisconnectTs = 0;
    uint64_t lastReceiveTs = 0;
    while(1){
        tLog("Tcp Source is Running");
        m.listenerActive = false;
        refresh_source_metadata(m, now_ms(), lastConnectionTs, lastDisconnectTs, lastReceiveTs);
        writeMetaData(m);
        if (tcpInstance.createConnection(TCP_IPORT, NULL, 0) < 0) {
            tLog("Tcp createConnection failed");
            ++m.receiveFailures;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        tLog("Created Tcp Connection");
        uint64_t now = now_ms();
        m.listenerActive = true;
        m.sourceCurrentlyConnected = true;
        m.sourceEverConnected = true;
        m.bytesTxThisConnection = 0;
        ++m.totalConnections;
        lastConnectionTs = now;
        refresh_source_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastReceiveTs);
        writeMetaData(m);
        std::array<uint8_t, 4096> ReadBuffer = {};
        while(1){
            int err = tcpInstance.readPacket(ReadBuffer);
            if(err > 0){
                now = now_ms();
                ++m.totalPacketsReceived;
                m.bytesTxThisConnection += static_cast<uint64_t>(err);
                m.totalBytesTransmitted += static_cast<uint64_t>(err);
                m.lastPacketBytes = static_cast<uint64_t>(err);
                lastReceiveTs = now;
                refresh_source_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastReceiveTs);
                TcpPacket packet{};
                packet.data = ReadBuffer;
                packet.len = static_cast<uint32_t>(err);
                writeFn(packet);
                writeMetaData(m);
                continue;
            }
            if(err == 0){
                tLog("Tcp Connection Lost");
                now = now_ms();
                m.sourceCurrentlyConnected = false;
                ++m.totalDisconnects;
                lastDisconnectTs = now;
                refresh_source_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastReceiveTs);
                writeMetaData(m);
                break;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR){
                refresh_source_metadata(m, now_ms(), lastConnectionTs, lastDisconnectTs, lastReceiveTs);
                writeMetaData(m);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            tLog("Tcp read failed");
            now = now_ms();
            ++m.receiveFailures;
            m.sourceCurrentlyConnected = false;
            ++m.totalDisconnects;
            lastDisconnectTs = now;
            refresh_source_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastReceiveTs);
            writeMetaData(m);
            break;
        }
        tcpInstance.closeConnection();
        m.listenerActive = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
};

void sourceThread(SPMCQueue<TcpPacket, 8>& tcpQueue, 
                  SPMCQueue<std::array<uint8_t, 4096>, 2>& udpQueue,
                  SPMCQueue<struct tcpSourceMetaData, 8>& tcpSourceStatusBuffer,
                  SPMCQueue<struct udpSourceMetaData, 8>& udpSourceStatusBuffer
                  ){
    tLog("Source Thread is Running");
    Tcp tcpInstance;
    Udp udpInstance;

    auto writeTCP = [&tcpQueue](const TcpPacket& buf){
        tcpQueue.write([&](TcpPacket& slot){ slot = buf; });
    };

    auto writeTCPMetaData = [&tcpSourceStatusBuffer](const struct tcpSourceMetaData& buf){
        tcpSourceStatusBuffer.write([&](struct tcpSourceMetaData& slot){ slot = buf; });
    };

    auto writeUDP = [&udpQueue](const std::array<uint8_t, 4096>& buf){
        udpQueue.write([&](std::array<uint8_t, 4096>& slot){ slot = buf; });
    };

    auto writeUDPMetaData = [&udpSourceStatusBuffer](const struct udpSourceMetaData& buf){
        udpSourceStatusBuffer.write([&](struct udpSourceMetaData& slot){ slot = buf; });
    };

    while(1){
        std::thread tcpSource_t(tcpSource, std::ref(tcpInstance), writeTCP, writeTCPMetaData);
        std::thread udpSource_t(udpSource, std::ref(udpInstance), writeUDP, writeUDPMetaData);

        tcpSource_t.join();
        udpSource_t.join();
        tLog("Source Threads Closed!");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
};

void tcpSink(SPMCQueue<TcpPacket, 8>& queue,
             std::function<void(const struct tcpClientMetaData&)> writeMetaData){
    SPMCQueue<TcpPacket, 8>::Reader tcpReader = queue.getReader();
    TcpPacket* tcpBuffer = {};
    EventPoll epoll{};
    Tcp tcpSocket;
    tcpClientMetaData m{};
    uint64_t lastConnectionTs = 0;
    uint64_t lastDisconnectTs = 0;
    uint64_t lastTransmissionTs = 0;

    tcpSocket.createConnection(TCP_OPORT, NULL, -1);
    int lfd = tcpSocket.sock;
    m.listenPort = TCP_OPORT;
    m.listenerActive = true;

    epoll.init(lfd);
    int tfd = epoll.tfd;
    refresh_client_metadata(m, now_ms(), lastConnectionTs, lastDisconnectTs, lastTransmissionTs);
    writeMetaData(m);

    while(true){
        int n = epoll.wait();
        for(int i = 0; i < n; i++){
            int fd = epoll.events[i].data.fd;
            if(fd == lfd){          // event from socket
                uint64_t accepted = epoll.handleConnection();
                if(accepted > 0){
                    uint64_t now = now_ms();
                    m.clientsCurrentlyConnected = epoll.clients.size();
                    m.totalClientConnections += accepted;
                    if(m.clientsCurrentlyConnected > m.peakClientConnections){
                        m.peakClientConnections = m.clientsCurrentlyConnected;
                    }
                    lastConnectionTs = now;
                    refresh_client_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastTransmissionTs);
                    writeMetaData(m);
                }
            } else if (fd == tfd) { // event from timer
                epoll.handleTimer();
                ++m.totalDispatchCycles;
                tcpBuffer = tcpReader.readLast();
                EventPoll::WriteResult result{};
                if(tcpBuffer != nullptr && tcpBuffer->len > 0){
                    for(auto it = epoll.clients.begin(); it != epoll.clients.end();){
                        int cfd = it->first;
                        ssize_t s = send(cfd, tcpBuffer->data.data(), tcpBuffer->len, MSG_NOSIGNAL);
                        if(s <= 0){
                            epoll_ctl(epoll.ep, EPOLL_CTL_DEL, cfd, nullptr);
                            close(cfd);
                            it = epoll.clients.erase(it);
                            ++result.transmission_failures;
                        } else {
                            ++it;
                            result.bytes_transmitted += static_cast<uint64_t>(s);
                            ++result.clients_written;
                            if(static_cast<size_t>(s) != tcpBuffer->len){
                                ++result.partial_writes;
                            }
                        }
                    }
                }
                uint64_t now = now_ms();
                m.clientsCurrentlyConnected = epoll.clients.size();
                m.totalBytesTransmitted += result.bytes_transmitted;
                m.totalPacketsTransmitted += result.clients_written;
                m.partialTransmissions += result.partial_writes;
                m.transmissionFailures += result.transmission_failures;
                m.lastWriteBytes = result.bytes_transmitted;
                m.lastWriteFanout = result.clients_written;
                if(result.clients_written == 0){
                    ++m.idleDispatchCycles;
                }
                if(result.bytes_transmitted > 0) lastTransmissionTs = now;
                if(result.transmission_failures > 0){
                    m.totalClientDisconnects += result.transmission_failures;
                    lastDisconnectTs = now;
                }
                refresh_client_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastTransmissionTs);
                writeMetaData(m);
            } else {                // disconnect client
                if(epoll.removeClient(i, fd)){
                    uint64_t now = now_ms();
                    m.clientsCurrentlyConnected = epoll.clients.size();
                    ++m.totalClientDisconnects;
                    lastDisconnectTs = now;
                    refresh_client_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastTransmissionTs);
                    writeMetaData(m);
                }
            }
        }
    }
};

void udpSink(SPMCQueue<std::array<uint8_t, 4096>, 2>&queue,
             std::function<void(const struct udpClientMetaData&)> writeMetaData){
    SPMCQueue<std::array<uint8_t, 4096>, 2>::Reader udpReader = queue.getReader();
    std::array<uint8_t, 4096> *udpBuffer = {};
    udpClientMetaData m{};
    uint64_t lastConnectionTs = 0;
    uint64_t lastDisconnectTs = 0;
    uint64_t lastTransmissionTs = 0;
    m.listenPort = UDP_OPORT;
    m.requiresSubscription = true;
    refresh_client_metadata(m, now_ms(), lastConnectionTs, lastDisconnectTs, lastTransmissionTs);
    writeMetaData(m);
    while(true){
        EventPoll epoll{};
        Udp udpSocket;

        udpSocket.createConnection(UDP_OPORT, NULL);
        int lfd = udpSocket.sock;
        m.listenerActive = true;

        epoll.init(lfd);
        int tfd = epoll.tfd;

        std::unordered_map<UdpClient, UdpClientState, UdpClientHash> clients;
        while(true){
            int n = epoll.wait();
            for(int i = 0; i < n; i++){
                int fd = epoll.events[i].data.fd;
                if(fd == lfd){          // event from udp socket
                    if(epoll.events[i].events & EPOLLERR){
                        drain_udp_errqueue(lfd, clients, &m.errorQueueDisconnects);
                    }
                    while(true){
                        sockaddr_in src{};
                        socklen_t slen = sizeof(src);
                        uint8_t buf[4096];
                        ssize_t bytes = recvfrom(lfd, buf, sizeof(buf), 0, (sockaddr*)&src, &slen);
                        if(bytes < 0){
                            if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                            tLog("Udp sink recvfrom failed");
                            break;
                        }
                        UdpClient c = {src.sin_addr.s_addr, src.sin_port};
                        if (starts_with(buf, bytes, UDP_UNSUBSCRIBE)) {
                            ++m.unsubscribeMessages;
                            if(clients.erase(c) > 0){
                                uint64_t now = now_ms();
                                m.clientsCurrentlyConnected = clients.size();
                                ++m.totalClientDisconnects;
                                lastDisconnectTs = now;
                                refresh_client_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastTransmissionTs);
                                writeMetaData(m);
                            }
                            continue;
                        }
                        if (starts_with(buf, bytes, UDP_SUBSCRIBE)) {
                            ++m.subscribeMessages;
                        }
                        bool inserted = clients.find(c) == clients.end();
                        clients[c] = UdpClientState{src, 0};
                        if(inserted){
                            uint64_t now = now_ms();
                            m.clientsCurrentlyConnected = clients.size();
                            ++m.totalClientConnections;
                            if(m.clientsCurrentlyConnected > m.peakClientConnections){
                                m.peakClientConnections = m.clientsCurrentlyConnected;
                            }
                            lastConnectionTs = now;
                            refresh_client_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastTransmissionTs);
                            writeMetaData(m);
                        }
                    }
                } else if (fd == tfd) { // event from timer
                    epoll.handleTimer();
                    ++m.totalDispatchCycles;
                    size_t beforeDrain = clients.size();
                    drain_udp_errqueue(lfd, clients, &m.errorQueueDisconnects);
                    if(clients.size() != beforeDrain){
                        uint64_t now = now_ms();
                        m.clientsCurrentlyConnected = clients.size();
                        m.totalClientDisconnects += beforeDrain - clients.size();
                        lastDisconnectTs = now;
                        refresh_client_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastTransmissionTs);
                        writeMetaData(m);
                    }

                    udpBuffer = udpReader.readLast();
                    if(udpBuffer != nullptr){
                        uint64_t writeFanout = 0;
                        uint64_t writeBytes = 0;
                        for(auto it = clients.begin(); it != clients.end();){
                            ssize_t s =
                                sendto(lfd, udpBuffer->data(), udpBuffer->size(), 0,
                                       reinterpret_cast<const sockaddr*>(&it->second.addr),
                                       sizeof(it->second.addr));
                            if(s < 0 && errno != EAGAIN && errno != EWOULDBLOCK){
                                ++m.transmissionFailures;
                                ++m.totalClientDisconnects;
                                lastDisconnectTs = now_ms();
                                it = clients.erase(it);
                            } else {
                                if(s > 0){
                                    uint64_t sent = static_cast<uint64_t>(s);
                                    m.totalBytesTransmitted += sent;
                                    ++m.totalPacketsTransmitted;
                                    ++writeFanout;
                                    writeBytes += sent;
                                    lastTransmissionTs = now_ms();
                                }
                                ++it;
                            }
                        }
                        m.lastWriteFanout = writeFanout;
                        m.lastWriteBytes = writeBytes;
                        if(writeFanout == 0){
                            ++m.idleDispatchCycles;
                        }
                    } else {
                        m.lastWriteFanout = 0;
                        m.lastWriteBytes = 0;
                        ++m.idleDispatchCycles;
                    }
                    uint64_t now = now_ms();
                    m.clientsCurrentlyConnected = clients.size();
                    refresh_client_metadata(m, now, lastConnectionTs, lastDisconnectTs, lastTransmissionTs);
                    writeMetaData(m);
                }
            }
        }
    }
};


void sinkThread(SPMCQueue<TcpPacket, 8>& tcpQueue,
                SPMCQueue<std::array<uint8_t, 4096>, 2>& udpQueue,
                SPMCQueue<struct tcpClientMetaData, 8>& tcpClientStatusBuffer,
                SPMCQueue<struct udpClientMetaData, 8>& udpClientStatusBuffer 
                ){
    tLog("Sink Thread is Running");
    auto writeTCPMetaData = [&tcpClientStatusBuffer](const struct tcpClientMetaData& buf){
        tcpClientStatusBuffer.write([&](struct tcpClientMetaData& slot){ slot = buf; });
    };

    auto writeUDPMetaData = [&udpClientStatusBuffer](const struct udpClientMetaData& buf){
        udpClientStatusBuffer.write([&](struct udpClientMetaData& slot){ slot = buf; });
    };

    std::thread tcpSink_t(tcpSink, std::ref(tcpQueue), writeTCPMetaData);
    std::thread udpSink_t(udpSink, std::ref(udpQueue), writeUDPMetaData);

    tcpSink_t.join();
    udpSink_t.join();
}

void obsThread(SPMCQueue<struct tcpSourceMetaData, 8>& tcpSourceStatusBuffer,
               SPMCQueue<struct udpSourceMetaData, 8>& udpSourceStatusBuffer,
               SPMCQueue<struct tcpClientMetaData, 8>& tcpClientStatusBuffer,
               SPMCQueue<struct udpClientMetaData, 8>& udpClientStatusBuffer
               ){
    EventPoll epoll{};
    Tcp tcpSocket;
    tcpSocket.createConnection(OBS_OPORT, NULL, -1);
    int lfd = tcpSocket.sock;
    uint64_t startedAt = now_ms();
    uint64_t lastObserverConnectionTs = 0;
    uint64_t lastObserverDisconnectTs = 0;
    uint64_t lastObserverPublishTs = 0;

    epoll.init(lfd);
    int tfd = epoll.tfd;
    tcpSourceMetaData* b0{}; udpSourceMetaData* b1{};
    tcpClientMetaData* b2{}; udpClientMetaData* b3{};
    auto r0 = tcpSourceStatusBuffer.getReader();
    auto r1 = udpSourceStatusBuffer.getReader();
    auto r2 = tcpClientStatusBuffer.getReader();
    auto r3 = udpClientStatusBuffer.getReader();
    struct metaData m{};
    m.serverMetaData.observerPort = OBS_OPORT;
    m.serverMetaData.observerListenerActive = true;
    while(true){
        int n = epoll.wait();
        for(int i = 0; i < n; i++){
            int fd = epoll.events[i].data.fd;
            if(fd == lfd){// event from socket
                uint64_t accepted = epoll.handleConnection();
                if(accepted > 0){
                    uint64_t now = now_ms();
                    m.serverMetaData.observerClientsCurrentlyConnected = epoll.clients.size();
                    m.serverMetaData.observerTotalConnections += accepted;
                    if(m.serverMetaData.observerClientsCurrentlyConnected > m.serverMetaData.observerPeakConnections){
                        m.serverMetaData.observerPeakConnections = m.serverMetaData.observerClientsCurrentlyConnected;
                    }
                    lastObserverConnectionTs = now;
                }
            } else if (fd == tfd) { // event from timer
                epoll.handleTimer();
                b0 = r0.readLast(); b1 = r1.readLast();
                b2 = r2.readLast(); b3 = r3.readLast();
                if(b0 != nullptr) m.tcpSourceMetaData = *b0;
                if(b1 != nullptr) m.udpSourceMetaData = *b1;
                if(b2 != nullptr) m.tcpClientMetaData = *b2;
                if(b3 != nullptr) m.udpClientMetaData = *b3;
                uint64_t now = now_ms();
                m.serverMetaData.uptime = elapsed_ms(startedAt, now);
                m.serverMetaData.observerClientsCurrentlyConnected = epoll.clients.size();
                m.serverMetaData.timeSinceLastObserverConnection = elapsed_ms(lastObserverConnectionTs, now);
                m.serverMetaData.timeSinceLastObserverDisconnect = elapsed_ms(lastObserverDisconnectTs, now);
                m.serverMetaData.timeSinceLastObserverPublish = elapsed_ms(lastObserverPublishTs, now);
                for(auto it = epoll.clients.begin(); it != epoll.clients.end();){
                    int cfd = it->first;
                    const uint8_t* p = reinterpret_cast<const uint8_t*>(&m);
                    size_t remaining = sizeof(m);
                    bool failed = false;
                    while(remaining > 0){
                        ssize_t n = send(cfd, p, remaining, MSG_NOSIGNAL);
                        if(n <= 0){
                            failed = true;
                            break;
                        }
                        p += n;
                        remaining -= static_cast<size_t>(n);
                    }
                    if(failed){
                        ++m.serverMetaData.observerTransmissionFailures;
                        ++m.serverMetaData.observerTotalDisconnects;
                        lastObserverDisconnectTs = now_ms();
                        epoll_ctl(epoll.ep, EPOLL_CTL_DEL, cfd, nullptr);
                        close(cfd);
                        it = epoll.clients.erase(it);
                        m.serverMetaData.observerClientsCurrentlyConnected = epoll.clients.size();
                    } else {
                        ++m.serverMetaData.observerFramesPublished;
                        lastObserverPublishTs = now_ms();
                        ++it;
                    }
                }
            } else { // disconnect client
                if(epoll.removeClient(i, fd)){
                    m.serverMetaData.observerClientsCurrentlyConnected = epoll.clients.size();
                    ++m.serverMetaData.observerTotalDisconnects;
                    lastObserverDisconnectTs = now_ms();
                }
            }
        }
    }
};

int main(){
    while(1){
        SPMCQueue<TcpPacket, 8> tcpBuffer = {};
        SPMCQueue<std::array<uint8_t, 4096>, 2> udpBuffer = {};

        SPMCQueue<struct tcpSourceMetaData, 8> tcpSourceStatusBuffer = {};
        SPMCQueue<struct udpSourceMetaData, 8> udpSourceStatusBuffer = {};

        SPMCQueue<struct tcpClientMetaData, 8> tcpClientStatusBuffer = {};
        SPMCQueue<struct udpClientMetaData, 8> udpClientStatusBuffer = {};

        std::thread source_t(sourceThread, std::ref(tcpBuffer), std::ref(udpBuffer),
                                           std::ref(tcpSourceStatusBuffer),
                                           std::ref(udpSourceStatusBuffer));

        std::thread sink_t  (sinkThread,   std::ref(tcpBuffer), std::ref(udpBuffer),
                                           std::ref(tcpClientStatusBuffer),
                                           std::ref(udpClientStatusBuffer));

        std::thread obs_t   (obsThread,    std::ref(tcpSourceStatusBuffer),
                                           std::ref(udpSourceStatusBuffer),
                                           std::ref(tcpClientStatusBuffer),
                                           std::ref(udpClientStatusBuffer));
        source_t.join();
        sink_t.join();
        obs_t.join();
   }
    return -1;
};
