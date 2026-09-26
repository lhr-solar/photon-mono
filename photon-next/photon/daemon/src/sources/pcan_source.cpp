#include "photon/source.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace photon {
namespace {

std::uint64_t unixNowMs() {
    using clock = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

void trimPending(std::vector<proto::CanMonitorItem>& items, std::size_t cap) {
    if (items.size() > cap) {
        items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(items.size() - cap));
    }
}

}  // namespace

PcanSource::PcanSource(pcan::Config cfg, DecodeBuffer* decode) : cfg_(cfg), decode_(decode) {}

PcanSource::~PcanSource() {
    stop();
}

bool PcanSource::open(std::string& error) {
    auto device = pcan::Device::open(cfg_, error);
    if (!device) {
        return false;
    }
    device_ = std::move(*device);
    return true;
}

void PcanSource::start() {
    running_ = true;
    haveAnchor_ = false;
    minTimeMs_ = 0;
    maxTimeMs_ = 0;
    totalBatches_ = 0;
    {
        std::lock_guard<std::mutex> lock(inboxMu_);
        inbox_.clear();
    }
    pendingCan_.clear();
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { readLoop(); });
    }
}

void PcanSource::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(apiMu_);
    device_.close();
}

void PcanSource::enqueue(const proto::CanMonitorItem& item) {
    std::lock_guard<std::mutex> lock(inboxMu_);
    inbox_.push_back(item);
    trimPending(inbox_, 1024);
}

void PcanSource::readLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        bool received = false;
        for (;;) {
            if (!running_.load(std::memory_order_relaxed)) {
                break;
            }
            pcan::Msg msg{};
            pcan::Timestamp ts{};
            pcan::Status status = pcan::kQrcvEmpty;
            {
                std::lock_guard<std::mutex> lock(apiMu_);
                if (!device_.valid()) {
                    break;
                }
                status = device_.read(msg, ts);
            }
            if (status == pcan::kQrcvEmpty) {
                break;
            }
            if (status != pcan::kOk) {
                break;
            }
            received = true;
            const auto type = msg.msgType;
            if ((type & static_cast<std::uint8_t>(pcan::MessageType::Rtr)) != 0 ||
                (type & static_cast<std::uint8_t>(pcan::MessageType::Status)) != 0) {
                continue;
            }
            if (!haveAnchor_) {
                anchorHwMs_ = pcan::hardwareTimestampMs(ts);
                anchorUnixMs_ = unixNowMs();
                haveAnchor_ = true;
            }
            proto::CanMonitorItem item{};
            item.canId = msg.id & 0x1FFFFFFFu;
            item.dlc = msg.len > 8 ? 8 : msg.len;
            if ((type & static_cast<std::uint8_t>(pcan::MessageType::Extended)) != 0) {
                item.flags = 0x01;
            }
            item.timestampMs = pcan::toUnixMs(ts, anchorUnixMs_, anchorHwMs_);
            std::memcpy(item.data, msg.data, 8);
            enqueue(item);
        }
        if (!received && running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void PcanSource::tick(std::uint64_t) {
    std::vector<proto::CanMonitorItem> batch;
    {
        std::lock_guard<std::mutex> lock(inboxMu_);
        batch.swap(inbox_);
    }
    for (const auto& item : batch) {
        if (minTimeMs_ == 0) {
            minTimeMs_ = item.timestampMs;
        }
        maxTimeMs_ = item.timestampMs;
        ++totalBatches_;
        if (decode_) {
            decode_->ingest(item);
        }
        pendingCan_.push_back(item);
    }
    trimPending(pendingCan_, 1024);
}

std::vector<proto::CanMonitorItem> PcanSource::takeCanFrames() {
    auto out = std::move(pendingCan_);
    pendingCan_.clear();
    return out;
}

proto::TimelineStatus PcanSource::timeline() const {
    proto::TimelineStatus status{};
    status.mode = static_cast<std::uint8_t>(proto::TimelineMode::Live);
    status.cursorMs = maxTimeMs_;
    status.minTimeMs = minTimeMs_;
    status.maxTimeMs = maxTimeMs_;
    status.totalBatches = totalBatches_;
    return status;
}

bool PcanSource::writeCan(std::uint32_t canId, std::uint8_t dlc, const std::uint8_t data[8], bool extended,
                          std::string& error) {
    if (cfg_.listenOnly) {
        error = "listen_only is enabled";
        return false;
    }
    pcan::Msg msg{};
    msg.id = canId & 0x1FFFFFFFu;
    msg.msgType = static_cast<std::uint8_t>(extended ? pcan::MessageType::Extended : pcan::MessageType::Standard);
    msg.len = dlc > 8 ? 8 : dlc;
    std::memcpy(msg.data, data, 8);
    pcan::Status status = 0xFFFFFFFFu;
    {
        std::lock_guard<std::mutex> lock(apiMu_);
        if (!device_.valid()) {
            error = pcan::kUnavailableMessage;
            return false;
        }
        status = device_.write(msg);
    }
    if (status != pcan::kOk) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "PCAN write failed (status 0x%X)", static_cast<unsigned>(status));
        error = buf;
        return false;
    }
    proto::CanMonitorItem item{};
    item.canId = msg.id;
    item.dlc = msg.len;
    item.flags = extended ? 0x01 : 0;
    item.timestampMs = unixNowMs();
    std::memcpy(item.data, data, 8);
    enqueue(item);
    return true;
}

}  // namespace photon
