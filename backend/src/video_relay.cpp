#include "photon/video_relay.hpp"

#include <cstdio>
#include <string_view>

namespace photon {
namespace {

constexpr std::uint64_t kBackoffMs[] = {200, 500, 1000, 2000, 5000};
constexpr int kBackoffCount = 5;
constexpr std::size_t kMaxPendingFrames = 64;

}  // namespace

VideoRelay::VideoRelay(std::string defaultHost, std::uint16_t defaultPort)
    : defaultHost_(std::move(defaultHost))
    , defaultPort_(defaultPort == 0 ? 6700 : defaultPort)
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
    const bool sourceChanged = host != host_ || port != port_ || mode != mode_ || startMs != startMs_;
    subscribers_.insert(clientId);
    host_ = std::move(host);
    port_ = port;
    mode_ = mode;
    startMs_ = startMs;
    statusChanged_ = true;
    if (sourceChanged) {
        disconnect("source change", connected_);
        nextAttemptMs_ = 0;
        backoffIndex_ = 0;
    }
}

void VideoRelay::unsubscribe(std::uint64_t clientId) {
    if (subscribers_.erase(clientId) == 0) {
        return;
    }
    statusChanged_ = true;
    if (subscribers_.empty()) {
        disconnect("no subscribers", connected_);
        nextAttemptMs_ = 0;
        backoffIndex_ = 0;
    }
}

bool VideoRelay::subscribed(std::uint64_t clientId) const { return subscribers_.contains(clientId); }

void VideoRelay::tick(std::uint64_t nowMs) {
    if (subscribers_.empty()) {
        return;
    }
    tryConnect(nowMs);
}

void VideoRelay::attachPoll(std::vector<net::PollFd>& fds) const {
    if (sock_.valid()) {
        fds.push_back(net::PollFd{sock_.native(), net::kPollIn, 0});
    }
}

void VideoRelay::onPoll(const std::vector<net::PollFd>& fds) {
    if (!sock_.valid()) {
        return;
    }
    for (const auto& fd : fds) {
        if (fd.fd == sock_.native() && (fd.revents & (net::kPollIn | net::kPollHup | net::kPollErr)) != 0) {
            readAvailable();
            return;
        }
    }
}

void VideoRelay::stop() {
    subscribers_.clear();
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
    if (sock_.valid()) {
        return;
    }
    if (nextAttemptMs_ != 0 && nowMs < nextAttemptMs_) {
        return;
    }
    sock_ = net::connectTcp(host_, port_);
    if (!sock_.valid()) {
        setConnected(false, "unavailable");
        const std::uint64_t wait = kBackoffMs[backoffIndex_];
        if (backoffIndex_ + 1 < kBackoffCount) {
            ++backoffIndex_;
        }
        nextAttemptMs_ = nowMs + wait;
        return;
    }

    const auto req = video::encodeRequest(wireMode(), startMs_);
    const int n = sock_.sendSome(req.data(), req.size());
    if (n != static_cast<int>(req.size())) {
        disconnect("subscribe write failed", false);
        nextAttemptMs_ = nowMs + kBackoffMs[backoffIndex_];
        return;
    }

    parser_.clear();
    backoffIndex_ = 0;
    nextAttemptMs_ = 0;
    loggedUnavailable_ = false;
    setConnected(true, "connected");
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
            nextAttemptMs_ = 0;
            return;
        }
        if (n == 0) {
            break;
        }
        parser_.feed(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
        if (parser_.failed()) {
            disconnect("bad framing", true);
            nextAttemptMs_ = 0;
            return;
        }
        while (auto frame = parser_.take()) {
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

std::uint8_t VideoRelay::wireMode() const {
    return mode_ == ctrl::VideoMode::Replay ? video::kModeReplay : video::kModeLive;
}

}  // namespace photon
