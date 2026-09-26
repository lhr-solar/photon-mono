#include "photon/source.hpp"

#include <cstdio>
#include <cstring>

namespace photon {

DaqTcpSource::DaqTcpSource(std::string host, std::uint16_t port, DecodeBuffer* decode, std::string password)
    : host_(std::move(host)), port_(port), decode_(decode), password_(std::move(password)) {}

void DaqTcpSource::start() {
    running_ = true;
    lastConnectAttemptMs_ = 0;
    minTimeMs_ = 0;
    maxTimeMs_ = 0;
    totalBatches_ = 0;
    pendingCan_.clear();
    parser_.clear();
    sock_.close();
}

void DaqTcpSource::stop() {
    running_ = false;
    sock_.close();
    parser_.clear();
}

void DaqTcpSource::tryConnect(std::uint64_t nowMs) {
    if (sock_.valid()) {
        return;
    }
    // Empty EOF (no CAN on the hop) used to retry every 1 s. That burns the
    // shared ClientGate on 127.0.0.1 (3 accepts/IP/sec) so video locate
    // never gets a slot. Wait 5 s after a silent close.
    const std::uint64_t waitMs = gotBatch_ ? 1000 : 5000;
    if (lastConnectAttemptMs_ != 0 && nowMs - lastConnectAttemptMs_ < waitMs) {
        return;
    }
    lastConnectAttemptMs_ = nowMs;
    gotBatch_ = false;
    sock_ = net::connectTcp(host_, port_);
    if (sock_.valid()) {
        if (!net::writePhotonAuth(sock_, password_)) {
            sock_.close();
            std::fprintf(stdout, "[photon-backend] daq_tcp auth failed %s:%u\n", host_.c_str(),
                         static_cast<unsigned>(port_));
            return;
        }
        std::fprintf(stdout, "[photon-backend] daq_tcp connected to %s:%u\n", host_.c_str(),
                     static_cast<unsigned>(port_));
    } else {
        std::fprintf(stdout, "[photon-backend] daq_tcp connect failed %s:%u (will retry)\n", host_.c_str(),
                     static_cast<unsigned>(port_));
    }
}

void DaqTcpSource::tick(std::uint64_t nowMs) {
    if (!running_) {
        return;
    }
    tryConnect(nowMs);
}

void DaqTcpSource::attachPoll(std::vector<net::PollFd>& fds) const {
    if (sock_.valid()) {
        fds.push_back(net::PollFd{sock_.native(), net::kPollIn, 0});
    }
}

void DaqTcpSource::onPoll(const std::vector<net::PollFd>& fds) {
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

void DaqTcpSource::readAvailable() {
    std::uint8_t buf[4096];
    for (;;) {
        const int n = sock_.recvSome(buf, sizeof(buf));
        if (n < 0) {
            std::fprintf(stdout, "[photon-backend] daq_tcp disconnected\n");
            sock_.close();
            parser_.clear();
            return;
        }
        if (n == 0) {
            break;
        }
        parser_.feed(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
        while (auto batch = parser_.take()) {
            gotBatch_ = true;
            if (minTimeMs_ == 0) {
                minTimeMs_ = batch->timestamp;
            }
            maxTimeMs_ = batch->timestamp;
            ++totalBatches_;
            for (std::uint16_t i = 0; i < batch->count; ++i) {
                const auto& p = batch->packets[i];
                proto::CanMonitorItem item{};
                item.canId = p.canId;
                item.dlc = p.dlc > 8 ? 8 : p.dlc;
                item.timestampMs = batch->timestamp;
                std::memcpy(item.data, p.data, 8);
                pendingCan_.push_back(item);
                if (decode_) {
                    decode_->ingest(item);
                }
            }
        }
    }
    if (pendingCan_.size() > 1024) {
        pendingCan_.erase(pendingCan_.begin(), pendingCan_.begin() + static_cast<std::ptrdiff_t>(pendingCan_.size() - 1024));
    }
}

std::vector<proto::CanMonitorItem> DaqTcpSource::takeCanFrames() {
    auto out = std::move(pendingCan_);
    pendingCan_.clear();
    return out;
}

proto::TimelineStatus DaqTcpSource::timeline() const {
    proto::TimelineStatus status{};
    status.mode = static_cast<std::uint8_t>(proto::TimelineMode::Live);
    status.cursorMs = maxTimeMs_;
    status.minTimeMs = minTimeMs_;
    status.maxTimeMs = maxTimeMs_;
    status.totalBatches = totalBatches_;
    return status;
}

}  // namespace photon
