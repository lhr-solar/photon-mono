#pragma once

#include "photon/control.hpp"
#include "photon/net.hpp"
#include "photon/video_framing.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace photon {

class VideoRelay {
public:
    VideoRelay(std::string defaultHost, std::uint16_t defaultPort);

    void subscribe(std::uint64_t clientId, ctrl::VideoMode mode, std::uint64_t startMs, std::string host,
                   std::uint16_t port);
    void unsubscribe(std::uint64_t clientId);
    bool subscribed(std::uint64_t clientId) const;
    bool hasSubscribers() const { return !subscribers_.empty(); }

    void tick(std::uint64_t nowMs);
    void attachPoll(std::vector<net::PollFd>& fds) const;
    void onPoll(const std::vector<net::PollFd>& fds);
    void stop();

    std::vector<video::ServerFrame> takeFrames();
    void noteDropped(std::uint64_t n = 1);

    ctrl::Message statusMessage() const;
    bool consumeStatusChanged();

    const std::string& host() const { return host_; }
    std::uint16_t port() const { return port_; }

private:
    std::string defaultHost_;
    std::uint16_t defaultPort_ = 6700;
    std::string host_;
    std::uint16_t port_ = 6700;
    ctrl::VideoMode mode_ = ctrl::VideoMode::Live;
    std::uint64_t startMs_ = 0;

    net::Socket sock_;
    video::StreamParser parser_;
    std::vector<video::ServerFrame> pending_;
    std::unordered_set<std::uint64_t> subscribers_;

    bool connected_ = false;
    bool loggedUnavailable_ = false;
    bool statusChanged_ = false;
    int backoffIndex_ = 0;
    std::uint64_t nextAttemptMs_ = 0;
    std::uint64_t lastTsMs_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t dropped_ = 0;

    void tryConnect(std::uint64_t nowMs);
    void disconnect(const char* why, bool log);
    void setConnected(bool connected, const char* why);
    void readAvailable();
    std::uint8_t wireMode() const;
};

}  // namespace photon
