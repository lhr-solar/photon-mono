#pragma once

#include "photon/control.hpp"
#include "photon/net.hpp"
#include "photon/video_framing.hpp"
#include "photon/video_index.hpp"
#include "photon/video_udp.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace photon {

class VideoRelay {
public:
    VideoRelay(std::string defaultHost, std::uint16_t defaultPort, std::uint16_t udpPort = 0);

    void subscribe(std::uint64_t clientId, ctrl::VideoMode mode, std::uint64_t startMs, std::string host,
                   std::uint16_t port);
    void unsubscribe(std::uint64_t clientId);
    bool subscribed(std::uint64_t clientId) const;
    bool hasSubscribers() const { return !subscribers_.empty(); }
    // Drop the live TCP and hold reconnect so locate/range can use the :6700
    // accept slot. Live connect/eof otherwise starves catalog GOP fetches.
    void pauseLiveForRpc();

    void tick(std::uint64_t nowMs);
    void attachPoll(std::vector<net::PollFd>& fds) const;
    void onPoll(const std::vector<net::PollFd>& fds);
    void stop();

    std::vector<video::ServerFrame> takeFrames();
    void noteDropped(std::uint64_t n = 1);

    ctrl::Message statusMessage() const;
    bool consumeStatusChanged();

    // Locate/range RPCs share an authenticated socket, separate from live.
    // locate: `body` is the reply without the u32 length prefix.
    bool locate(std::uint64_t atMs, std::uint8_t streamId, std::uint16_t prefetchGops,
                std::vector<std::uint8_t>& body, std::string* error);
    // Mode 2: framed NALs from the keyframe at startMs through endMs.
    bool replayWindow(std::uint64_t startMs, std::uint64_t endMs, std::vector<video::ServerFrame>& frames,
                      std::string* error);
    // One live TCP: keep whatever framed NALs arrive before EOF. Used to fill
    // the per-client cache when locate/range is refused.
    bool grabLatest(std::vector<video::ServerFrame>& frames, std::string* error);
    // Mode 4: raw segment bytes for a GOP pointer.
    bool readRange(std::uint32_t segmentId, std::uint64_t offset, std::uint32_t length,
                   std::vector<std::uint8_t>& body, std::string* error);
    // Mode 5: persist the manual A/V nudge on photon-server.
    bool setClockOffset(std::int64_t offsetMs, std::int64_t& appliedMs, vidx::ClockProv& provenance,
                        std::string* error);

    const std::string& host() const { return host_; }
    std::uint16_t port() const { return port_; }
    void setPassword(std::string password) {
        if (password != password_) rpcSock_.close();
        password_ = std::move(password);
    }
    void setUdpHost(std::string host) {
        if (!host.empty()) {
            udpHost_ = std::move(host);
        }
    }

private:
    std::string defaultHost_;
    std::uint16_t defaultPort_ = 6700;
    std::uint16_t udpPort_ = 0;
    std::string udpHost_;
    std::string host_;
    std::uint16_t port_ = 6700;
    ctrl::VideoMode mode_ = ctrl::VideoMode::Live;
    std::uint64_t startMs_ = 0;

    net::Socket sock_;
    net::Socket rpcSock_;
    net::Socket udp_;
    video::UdpAssembler udpAsm_;
    std::uint64_t lastUdpSubscribeMs_ = 0;
    bool requestPointer(std::span<const std::uint8_t> request, std::vector<std::uint8_t>& body,
                        std::string* error);
    video::StreamParser parser_;
    std::vector<video::ServerFrame> pending_;
    std::unordered_set<std::uint64_t> subscribers_;

    bool connected_ = false;
    bool loggedUnavailable_ = false;
    bool statusChanged_ = false;
    bool gotFrame_ = false;
    int backoffIndex_ = 0;
    std::uint64_t nextAttemptMs_ = 0;
    std::uint64_t lastTickMs_ = 0;
    std::uint64_t lastPointerFailMs_ = 0;
    std::uint64_t lastTsMs_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t dropped_ = 0;
    std::string password_;

    void tryConnect(std::uint64_t nowMs);
    void disconnect(const char* why, bool log);
    void setConnected(bool connected, const char* why);
    void readAvailable();
    void readUdp();
    void sendUdpSubscribe(bool on);
    void armBackoff();
};

}  // namespace photon
