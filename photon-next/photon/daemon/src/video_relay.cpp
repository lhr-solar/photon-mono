#include "photon/video_relay.hpp"

#include "photon/video_index.hpp"
#include "photon/video_udp.hpp"

#include <chrono>
#include <cstdio>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

namespace photon {
namespace {

constexpr std::uint64_t kBackoffMs[] = {200, 500, 1000, 2000, 5000};
constexpr std::uint64_t kEmptyLiveBackoffMs = 5000;
constexpr int kBackoffCount = 5;
constexpr std::size_t kMaxPendingFrames = 64;

}  // namespace

VideoRelay::VideoRelay(std::string defaultHost, std::uint16_t defaultPort, std::uint16_t udpPort)
    : defaultHost_(std::move(defaultHost))
    , defaultPort_(defaultPort == 0 ? 6700 : defaultPort)
    , udpPort_(udpPort)
    , host_(defaultHost_)
    , port_(defaultPort_) {}

void VideoRelay::subscribe(std::uint64_t clientId, ctrl::VideoMode mode, std::uint64_t startMs, std::string host,
                           std::uint16_t port) {
    if (host.empty()) {
        host = defaultHost_;
    }
    if (port == 0) {
        port = defaultPort_;
    }
    subscribers_.insert(clientId);
    host_ = std::move(host);
    port_ = port;
    mode_ = mode;
    startMs_ = startMs;
    statusChanged_ = true;
}

void VideoRelay::unsubscribe(std::uint64_t clientId) {
    if (subscribers_.erase(clientId) == 0) {
        return;
    }
    statusChanged_ = true;
    if (subscribers_.empty()) {
        disconnect("no subscribers", connected_ && udpPort_ == 0);
        nextAttemptMs_ = 0;
        backoffIndex_ = 0;
        lastPointerFailMs_ = 0;
    }
}

bool VideoRelay::subscribed(std::uint64_t clientId) const { return subscribers_.contains(clientId); }

void VideoRelay::pauseLiveForRpc() {
    // Keep lastPointerFailMs_. Clearing it here let every tab's video_locate
    // reconnect immediately and trip ClientGate (3 accepts/IP/s).
    if (rpcSock_.valid()) {
        return;
    }
    if (sock_.valid()) {
        disconnect("locate rpc", connected_);
    }
    std::uint64_t now = lastTickMs_;
    if (now == 0) {
        now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());
    }
    nextAttemptMs_ = now + 1500;
}

void VideoRelay::tick(std::uint64_t nowMs) {
    lastTickMs_ = nowMs;
    // Keep the Oracle/local UDP join up even with no UI yet. Waiting for
    // video_locate 0 left live video dark whenever hello was late.
    if (udpPort_ != 0 && !password_.empty()) {
        if (!udp_.valid()) {
            udp_ = net::bindUdp(0);
            lastUdpSubscribeMs_ = 0;
            if (udp_.valid()) {
                const std::string& dest = udpHost_.empty() ? host_ : udpHost_;
                std::fprintf(stdout, "[photon-backend] video udp -> %s:%u\n", dest.c_str(),
                             static_cast<unsigned>(udpPort_));
                std::fflush(stdout);
            } else {
                std::fprintf(stderr, "[photon-backend] video udp bind failed\n");
            }
        }
        if (udp_.valid() && (lastUdpSubscribeMs_ == 0 || nowMs - lastUdpSubscribeMs_ >= 400)) {
            sendUdpSubscribe(true);
            lastUdpSubscribeMs_ = nowMs;
        }
        if (udp_.valid()) {
            readUdp();
        }
    }
    tryConnect(nowMs);
}

void VideoRelay::attachPoll(std::vector<net::PollFd>& fds) const {
    if (sock_.valid()) {
        fds.push_back(net::PollFd{sock_.native(), net::kPollIn, 0});
    }
    if (udp_.valid()) {
        fds.push_back(net::PollFd{udp_.native(), net::kPollIn, 0});
    }
}

void VideoRelay::onPoll(const std::vector<net::PollFd>& fds) {
    bool udpReady = false;
    bool tcpReady = false;
    for (const auto& fd : fds) {
        if (udp_.valid() && fd.fd == udp_.native() && (fd.revents & net::kPollIn) != 0) {
            udpReady = true;
        }
        if (sock_.valid() && fd.fd == sock_.native() &&
            (fd.revents & (net::kPollIn | net::kPollHup | net::kPollErr)) != 0) {
            tcpReady = true;
        }
    }
    if (udpReady) {
        readUdp();
    }
    if (tcpReady) {
        readAvailable();
    }
}

void VideoRelay::stop() {
    rpcSock_.close();
    subscribers_.clear();
    if (udp_.valid()) {
        sendUdpSubscribe(false);
        udp_.close();
    }
    udpAsm_.clear();
    disconnect("server_stop", connected_);
    pending_.clear();
    parser_.clear();
}

std::vector<video::ServerFrame> VideoRelay::takeFrames() {
    auto out = std::move(pending_);
    pending_.clear();
    return out;
}

void VideoRelay::noteDropped(std::uint64_t n) {
    dropped_ += n;
    statusChanged_ = true;
}

ctrl::Message VideoRelay::statusMessage() const {
    return ctrl::videoStatus(connected_, host_, port_, mode_, lastTsMs_, frames_, dropped_);
}

bool VideoRelay::consumeStatusChanged() {
    const bool changed = statusChanged_;
    statusChanged_ = false;
    return changed;
}

void VideoRelay::tryConnect(std::uint64_t nowMs) {
    (void)nowMs;
    // Live pictures come from UDP :6800 (and video_locate seed). A Live-mode
    // TCP to :6700 connect/eof pair burns ClientGate (3 accepts/IP/s) and
    // makes every later locate look like "video pointer request failed".
}

void VideoRelay::sendUdpSubscribe(bool on) {
    if (!udp_.valid() || udpPort_ == 0 || password_.empty()) {
        return;
    }
    const auto pkt = video::encodeUdpSubscribe(password_, on);
    if (pkt.empty()) {
        return;
    }
    const std::string& dest = udpHost_.empty() ? host_ : udpHost_;
    udp_.sendTo(dest, udpPort_, pkt.data(), pkt.size());
}

void VideoRelay::readUdp() {
    if (!udp_.valid()) {
        return;
    }
    std::uint8_t buf[2048];
    for (;;) {
        const int n = udp_.recvFrom(buf, sizeof(buf), nullptr, nullptr);
        if (n <= 0) {
            break;
        }
        video::UdpPacket pkt;
        if (!video::decodeUdpPacket(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)), pkt)) {
            continue;
        }
        auto frame = udpAsm_.add(pkt);
        if (!frame) {
            continue;
        }
        if (pending_.size() >= kMaxPendingFrames) {
            pending_.erase(pending_.begin());
            ++dropped_;
        }
        lastTsMs_ = frame->timestampMs;
        ++frames_;
        gotFrame_ = true;
        if (!connected_) {
            setConnected(true, "udp");
        }
        if (frames_ == 1 || frames_ % 200 == 0) {
            std::fprintf(stdout, "[photon-backend] video udp frames=%llu last=%llu flags=%u bytes=%zu\n",
                         static_cast<unsigned long long>(frames_),
                         static_cast<unsigned long long>(frame->timestampMs),
                         static_cast<unsigned>(frame->flags), frame->payload.size());
            std::fflush(stdout);
        }
        pending_.push_back(std::move(*frame));
        statusChanged_ = true;
    }
}

void VideoRelay::armBackoff() {
    const std::uint64_t wait = gotFrame_ ? kBackoffMs[backoffIndex_] : kEmptyLiveBackoffMs;
    if (backoffIndex_ + 1 < kBackoffCount) {
        ++backoffIndex_;
    }
    nextAttemptMs_ = lastTickMs_ + wait;
}

void VideoRelay::disconnect(const char* why, bool log) {
    if (sock_.valid()) {
        sock_.close();
    }
    parser_.clear();
    if (connected_) {
        setConnected(false, why);
    } else if (log) {
        std::fprintf(stdout, "[photon-backend] video disconnected (%s)\n", why);
    }
}

void VideoRelay::setConnected(bool connected, const char* why) {
    if (connected_ == connected) {
        if (!connected && why != nullptr && std::string_view(why) == "unavailable" && !loggedUnavailable_) {
            std::fprintf(stdout, "[photon-backend] video unavailable %s:%u\n", host_.c_str(),
                         static_cast<unsigned>(port_));
            loggedUnavailable_ = true;
        }
        return;
    }
    connected_ = connected;
    statusChanged_ = true;
    if (connected) {
        std::fprintf(stdout, "[photon-backend] video connected %s:%u mode=%s\n", host_.c_str(),
                     static_cast<unsigned>(port_), std::string(ctrl::videoModeName(mode_)).c_str());
    } else if (why != nullptr && std::string_view(why) == "unavailable") {
        if (!loggedUnavailable_) {
            std::fprintf(stdout, "[photon-backend] video unavailable %s:%u\n", host_.c_str(),
                         static_cast<unsigned>(port_));
            loggedUnavailable_ = true;
        }
    } else {
        std::fprintf(stdout, "[photon-backend] video disconnected (%s)\n", why != nullptr ? why : "closed");
    }
}

void VideoRelay::readAvailable() {
    std::uint8_t buf[4096];
    for (;;) {
        const int n = sock_.recvSome(buf, sizeof(buf));
        if (n < 0) {
            disconnect("eof", true);
            armBackoff();
            return;
        }
        if (n == 0) {
            break;
        }
        parser_.feed(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
        if (parser_.failed()) {
            disconnect("bad framing", true);
            armBackoff();
            return;
        }
        while (auto frame = parser_.take()) {
            gotFrame_ = true;
            backoffIndex_ = 0;
            lastTsMs_ = frame->timestampMs;
            ++frames_;
            statusChanged_ = true;
            pending_.push_back(std::move(*frame));
        }
    }
    if (pending_.size() > kMaxPendingFrames) {
        // Keep config + latest keyframe tail; drop older deltas.
        std::vector<video::ServerFrame> kept;
        kept.reserve(kMaxPendingFrames);
        for (auto& frame : pending_) {
            if (video::isConfig(frame.flags) || video::isKeyframe(frame.flags)) {
                kept.push_back(std::move(frame));
            } else {
                ++dropped_;
            }
        }
        if (kept.size() > kMaxPendingFrames) {
            kept.erase(kept.begin(), kept.end() - static_cast<std::ptrdiff_t>(kMaxPendingFrames));
        }
        pending_ = std::move(kept);
    }
}

namespace {

bool sendAll(const net::Socket& sock, const std::uint8_t* data, std::size_t len) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    std::size_t off = 0;
    while (off < len) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        const int n = sock.sendSome(data + off, len - off);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool recvAll(const net::Socket& sock, std::uint8_t* data, std::size_t len) {
    // Keep the socket non-blocking. A blocking recv on a Live-mode peer
    // (no pictures, or an old server that ignored locate) froze the hub.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    std::size_t off = 0;
    while (off < len) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        const int n = sock.recvSome(data + off, len - off);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

bool VideoRelay::locate(std::uint64_t atMs, std::uint8_t streamId, std::uint16_t prefetchGops,
                        std::vector<std::uint8_t>& body, std::string* error) {
    const auto req = vidx::encodeLocateRequest(atMs, streamId, prefetchGops == 0 ? 4 : prefetchGops);
    return requestPointer(req, body, error);
}

bool VideoRelay::requestPointer(std::span<const std::uint8_t> request,
                                std::vector<std::uint8_t>& body, std::string* error) {
    body.clear();
    const auto nowMs = lastTickMs_ != 0
                           ? lastTickMs_
                           : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
    // A 400ms UI locate loop plus reconnects trips ClientGate (3 accepts/IP/s).
    if (!rpcSock_.valid() && lastPointerFailMs_ != 0 && nowMs >= lastPointerFailMs_ &&
        nowMs - lastPointerFailMs_ < 2500) {
        if (error) {
            *error = "video pointer busy";
        }
        return false;
    }
    // One retry covers a stale retained socket or a first-connect race with
    // live TCP. The 2.5 s lockout still blocks a new call after both fail.
    const int attempts = 2;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (!rpcSock_.valid()) {
            rpcSock_ = net::connectTcpTimed(host_, port_, 2500);
            if (!rpcSock_.valid() || !net::writePhotonAuth(rpcSock_, password_)) {
                rpcSock_.close();
                lastPointerFailMs_ = nowMs;
                if (error) *error = "video service connection/authentication failed";
                return false;
            }
        }
        std::uint8_t lenBuf[4];
        const bool sent = sendAll(rpcSock_, request.data(), request.size());
        const bool gotHeader = sent && recvAll(rpcSock_, lenBuf, 4);
        if (!gotHeader) {
            rpcSock_.close();
            if (error) {
                *error = sent ? "video store closed the pointer" : "video pointer write failed";
            }
            if (attempt + 1 < attempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                continue;
            }
            lastPointerFailMs_ = nowMs;
            return false;
        }
        const std::uint32_t n = (static_cast<std::uint32_t>(lenBuf[0]) << 24) |
                               (static_cast<std::uint32_t>(lenBuf[1]) << 16) |
                               (static_cast<std::uint32_t>(lenBuf[2]) << 8) | lenBuf[3];
        if (n > 32u * 1024u * 1024u) {
            rpcSock_.close();
            lastPointerFailMs_ = nowMs;
            if (error) *error = "video pointer reply too large";
            return false;
        }
        body.resize(n);
        if (n == 0 || recvAll(rpcSock_, body.data(), n)) {
            lastPointerFailMs_ = 0;
            return true;
        }
        body.clear();
        rpcSock_.close();
        if (error) {
            *error = "video pointer body read failed";
        }
    }
    lastPointerFailMs_ = nowMs;
    return false;
}

bool VideoRelay::replayWindow(std::uint64_t startMs, std::uint64_t endMs, std::vector<video::ServerFrame>& frames,
                              std::string* error) {
    frames.clear();
    if (endMs < startMs) {
        std::swap(endMs, startMs);
    }
    if (endMs - startMs > 60'000ull) {
        endMs = startMs + 60'000ull;
    }
    auto sock = net::connectTcpTimed(host_, port_, 2500);
    if (!sock.valid()) {
        if (error) {
            *error = "video host unavailable";
        }
        return false;
    }
    if (!net::writePhotonAuth(sock, password_)) {
        if (error) {
            *error = "video auth failed";
        }
        return false;
    }
    const auto req = vidx::encodeReplayRangeRequest(startMs, endMs);
    if (!sendAll(sock, req.data(), req.size())) {
        if (error) {
            *error = "video replay-range write failed";
        }
        return false;
    }
    video::StreamParser parser;
    std::uint8_t buf[16 * 1024];
    std::size_t bytes = 0;
    constexpr std::size_t kMaxBytes = vidx::kReplayCacheBudget;
    for (int spin = 0; spin < 4000; ++spin) {
        const int n = sock.recvSome(buf, sizeof(buf));
        if (n < 0) {
            break;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        bytes += static_cast<std::size_t>(n);
        parser.feed(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
        if (parser.failed() || bytes > kMaxBytes) {
            break;
        }
    }
    while (auto frame = parser.take()) {
        frames.push_back(std::move(*frame));
        if (frames.size() >= 8000) {
            break;
        }
    }
    return true;
}

bool VideoRelay::grabLatest(std::vector<video::ServerFrame>& frames, std::string* error) {
    frames.clear();
    auto sock = net::connectTcpTimed(host_, port_, 2500);
    if (!sock.valid()) {
        if (error) {
            *error = "video host unavailable";
        }
        return false;
    }
    if (!net::writePhotonAuth(sock, password_)) {
        if (error) {
            *error = "video auth failed";
        }
        return false;
    }
    const auto req = video::encodeRequest(video::kModeLive, 0);
    if (!sendAll(sock, req.data(), req.size())) {
        if (error) {
            *error = "video live write failed";
        }
        return false;
    }
    video::StreamParser parser;
    std::uint8_t buf[16 * 1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        const int n = sock.recvSome(buf, sizeof(buf));
        if (n < 0) {
            break;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        parser.feed(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
        if (parser.failed()) {
            break;
        }
    }
    while (auto frame = parser.take()) {
        frames.push_back(std::move(*frame));
        if (frames.size() >= 64) {
            break;
        }
    }
    if (frames.empty()) {
        if (error) {
            *error = "video live returned no NALs";
        }
        return false;
    }
    return true;
}

bool VideoRelay::readRange(std::uint32_t segmentId, std::uint64_t offset, std::uint32_t length,
                           std::vector<std::uint8_t>& body, std::string* error) {
    const auto req = vidx::encodeRangeRequest(segmentId, offset, length);
    return requestPointer(req, body, error);
}

bool VideoRelay::setClockOffset(std::int64_t offsetMs, std::int64_t& appliedMs, vidx::ClockProv& provenance,
                                std::string* error) {
    appliedMs = 0;
    provenance = vidx::ClockProv::Receive;
    auto sock = net::connectTcpTimed(host_, port_, 2500);
    if (!sock.valid()) {
        if (error) {
            *error = "video host unavailable";
        }
        return false;
    }
    if (!net::writePhotonAuth(sock, password_)) {
        if (error) {
            *error = "video auth failed";
        }
        return false;
    }
    const auto req = vidx::encodeClockRequest(offsetMs);
    if (!sendAll(sock, req.data(), req.size())) {
        if (error) {
            *error = "video_clock write failed";
        }
        return false;
    }
    std::uint8_t lenBuf[4];
    if (!recvAll(sock, lenBuf, 4)) {
        if (error) {
            *error = "video_clock header read failed";
        }
        return false;
    }
    const std::uint32_t n = (static_cast<std::uint32_t>(lenBuf[0]) << 24) |
                            (static_cast<std::uint32_t>(lenBuf[1]) << 16) |
                            (static_cast<std::uint32_t>(lenBuf[2]) << 8) | static_cast<std::uint32_t>(lenBuf[3]);
    if (n == 0 || n > 64) {
        if (error) {
            *error = "video_clock reply too large";
        }
        return false;
    }
    std::vector<std::uint8_t> body(n);
    if (!recvAll(sock, body.data(), n)) {
        if (error) {
            *error = "video_clock body read failed";
        }
        return false;
    }
    return vidx::decodeClockReply(body, appliedMs, provenance);
}

}  // namespace photon
