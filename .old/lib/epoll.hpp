#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>

#include "shared.hpp"

static inline void set_tcp_low_latency_epoll(int fd){
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

struct EventPoll {
    struct WriteResult {
        uint64_t bytes_transmitted{};
        uint64_t clients_written{};
        uint64_t transmission_failures{};
        uint64_t partial_writes{};
    };

    void init(int fd){
        events.resize(64);
        lfd = fd;
        ep = epoll_create1(0);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = lfd;
        epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);

        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        itimerspec ts{};
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 1'953'125;
        ts.it_value = ts.it_interval;
        timerfd_settime(tfd, 0, &ts, nullptr);

        ev.events = EPOLLIN;
        ev.data.fd = tfd;
        epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
    }

    int wait(){
        return epoll_wait(ep, events.data(), events.size(), -1);
    };

    uint64_t handleConnection(){
        uint64_t accepted = 0;
        while(true){
            int cfd = accept(lfd, nullptr, nullptr);
            if (cfd < 0) return accepted;
            set_nonblock(cfd);
            set_tcp_low_latency_epoll(cfd);
            epoll_event cev{};
            cev.events = EPOLLRDHUP;
            cev.data.fd = cfd;
            epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);
            clients[cfd] = true;
            ++accepted;
        }
    }

    void handleTimer(){
        uint64_t tmp;
        read(tfd, &tmp, sizeof(tmp));
    };

    WriteResult writeClients(std::array<uint8_t, 4096>* dataBuffer){
        WriteResult result{};
        if(dataBuffer != nullptr){
            for(auto it = clients.begin(); it != clients.end();){
                int cfd = it->first;
                ssize_t s = send(cfd, dataBuffer->data(), dataBuffer->size(), MSG_NOSIGNAL);
                if(s <= 0){
                    epoll_ctl(ep, EPOLL_CTL_DEL, cfd, nullptr);
                    close(cfd);
                    it = clients.erase(it);
                    ++result.transmission_failures;
                } else {
                    ++it;
                    result.bytes_transmitted += static_cast<uint64_t>(s);
                    ++result.clients_written;
                    if(static_cast<size_t>(s) != dataBuffer->size()){
                        ++result.partial_writes;
                    }
                }
            }
        }
        return result;
    }

    bool removeClient(uint32_t eventIndex, int fd){
        uint32_t event = events[eventIndex].events;
        if(event & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)){
            epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            clients.erase(fd);
            return true;
        }
        return false;
    }

    std::unordered_map<int, bool> clients;
    std::unordered_map<int, sockaddr_in> udpClients;
    std::vector<epoll_event> events;
    int ep = -1;
    int lfd = -1;
    int tfd = -1;
};
