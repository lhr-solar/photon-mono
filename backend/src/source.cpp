#include "photon/source.hpp"

#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace photon {
namespace {

constexpr std::size_t kRingCap = 512;
constexpr std::uint64_t kGenPeriodMs = 10;  // 100 Hz

double nowSeconds(std::uint64_t nowMs, std::uint64_t startMs) {
    return static_cast<double>(nowMs - startMs) / 1000.0;
}

proto::CanMonitorItem makeCan(std::uint32_t canId, std::uint64_t timestampMs, const std::uint8_t data[8],
                              std::uint8_t dlc) {
    proto::CanMonitorItem item{};
    item.canId = canId;
    item.dlc = dlc;
    item.timestampMs = timestampMs;
    std::memcpy(item.data, data, 8);
    return item;
}

}  // namespace

void DecodeBuffer::setDatabase(const dbc::Database* db) {
    db_ = db;
    clear();
}

void DecodeBuffer::clear() {
    rings_.clear();
}

void DecodeBuffer::ingest(const proto::CanMonitorItem& item) {
    if (db_ == nullptr) {
        return;
    }
    const auto samples = dbc::decodeFrame(*db_, item.canId, item.data, item.dlc);
    for (const auto& sample : samples) {
        SignalRing& ring = rings_[sample.signalId];
        ring.id = sample.signalId;
        ring.canId = item.canId;
        ring.samples.push_back(SignalSample{static_cast<double>(item.timestampMs), sample.value});
        while (ring.samples.size() > kRingCap) {
            ring.samples.pop_front();
            if (ring.lastEmitted > 0) {
                --ring.lastEmitted;
            }
        }
    }
}

std::vector<proto::TelemetryBlock> DecodeBuffer::takeTelemetry(const std::unordered_set<std::uint32_t>& ids) {
    std::vector<proto::TelemetryBlock> blocks;
    for (auto& [id, ring] : rings_) {
        if (!ids.contains(id)) {
            continue;
        }
        proto::TelemetryBlock block;
        block.signalId = id;
        for (std::size_t i = ring.lastEmitted; i < ring.samples.size(); ++i) {
            block.timestamps.push_back(ring.samples[i].timestampMs);
            block.values.push_back(ring.samples[i].value);
        }
        ring.lastEmitted = ring.samples.size();
        if (!block.timestamps.empty()) {
            blocks.push_back(std::move(block));
        }
    }
    return blocks;
}

SyntheticSource::SyntheticSource() {
    signals_.push_back(SignalRing{1, 0x100, "rpm", {}, 0});
    signals_.push_back(SignalRing{2, 0x101, "speed", {}, 0});
    signals_.push_back(SignalRing{3, 0x102, "soc", {}, 0});
    signals_.push_back(SignalRing{4, 0x103, "temp", {}, 0});
}

void SyntheticSource::start() {
    running_ = true;
    startMs_ = 0;
    lastGenMs_ = 0;
    minTimeMs_ = 0;
    maxTimeMs_ = 0;
    totalBatches_ = 0;
    stepPhase_ = 0;
    lastStepBucket_ = -1;
    pendingCan_.clear();
    for (auto& ring : signals_) {
        ring.samples.clear();
        ring.lastEmitted = 0;
    }
}

void SyntheticSource::stop() {
    running_ = false;
}

void SyntheticSource::pushSample(SignalRing& ring, double tMs, double value) {
    ring.samples.push_back(SignalSample{tMs, value});
    while (ring.samples.size() > kRingCap) {
        ring.samples.pop_front();
        if (ring.lastEmitted > 0) {
            --ring.lastEmitted;
        }
    }
}

void SyntheticSource::tick(std::uint64_t nowMs) {
    if (!running_) {
        return;
    }
    if (startMs_ == 0) {
        startMs_ = nowMs;
        lastGenMs_ = nowMs;
        minTimeMs_ = nowMs;
    }
    if (nowMs - lastGenMs_ < kGenPeriodMs) {
        return;
    }
    // Catch up a bounded number of 100 Hz ticks if the loop stalled.
    int ticks = 0;
    while (lastGenMs_ + kGenPeriodMs <= nowMs && ticks < 8) {
        lastGenMs_ += kGenPeriodMs;
        ++ticks;
        const double t = nowSeconds(lastGenMs_, startMs_);
        const double tMs = static_cast<double>(lastGenMs_);

        pushSample(signals_[0], tMs, 4000.0 + 4000.0 * std::sin(2.0 * 3.141592653589793 * 0.5 * t));
        pushSample(signals_[1], tMs, 20.0 + 20.0 * std::sin(2.0 * 3.141592653589793 * 0.2 * t));
        const int stepBucket = static_cast<int>(t) / 2;
        if (stepBucket != lastStepBucket_) {
            lastStepBucket_ = stepBucket;
            stepPhase_ = (stepPhase_ + 1) % 5;
        }
        pushSample(signals_[2], tMs, 20.0 + 20.0 * static_cast<double>(stepPhase_));
        pushSample(signals_[3], tMs, 50.0 + 30.0 * std::sin(2.0 * 3.141592653589793 * 0.1 * t));

        for (const auto& ring : signals_) {
            if (ring.samples.empty()) {
                continue;
            }
            const double value = ring.samples.back().value;
            std::uint8_t data[8]{};
            float f = static_cast<float>(value);
            std::memcpy(data, &f, sizeof(f));
            pendingCan_.push_back(makeCan(ring.canId, lastGenMs_, data, 8));
        }
        maxTimeMs_ = lastGenMs_;
        ++totalBatches_;
    }
    if (pendingCan_.size() > 256) {
        pendingCan_.erase(pendingCan_.begin(), pendingCan_.begin() + static_cast<std::ptrdiff_t>(pendingCan_.size() - 256));
    }
}

std::vector<proto::TelemetryBlock> SyntheticSource::takeTelemetry(const std::unordered_set<std::uint32_t>& ids) {
    std::vector<proto::TelemetryBlock> blocks;
    for (auto& ring : signals_) {
        if (!ids.contains(ring.id)) {
            continue;
        }
        proto::TelemetryBlock block;
        block.signalId = ring.id;
        for (std::size_t i = ring.lastEmitted; i < ring.samples.size(); ++i) {
            block.timestamps.push_back(ring.samples[i].timestampMs);
            block.values.push_back(ring.samples[i].value);
        }
        ring.lastEmitted = ring.samples.size();
        if (!block.timestamps.empty()) {
            blocks.push_back(std::move(block));
        }
    }
    return blocks;
}

std::vector<proto::CanMonitorItem> SyntheticSource::takeCanFrames() {
    auto out = std::move(pendingCan_);
    pendingCan_.clear();
    return out;
}

proto::TimelineStatus SyntheticSource::timeline() const {
    proto::TimelineStatus status{};
    status.mode = static_cast<std::uint8_t>(proto::TimelineMode::Live);
    status.cursorMs = maxTimeMs_;
    status.minTimeMs = minTimeMs_;
    status.maxTimeMs = maxTimeMs_;
    status.totalBatches = totalBatches_;
    return status;
}

DaqTcpSource::DaqTcpSource(std::string host, std::uint16_t port, DecodeBuffer* decode)
    : host_(std::move(host)), port_(port), decode_(decode) {}

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
    if (lastConnectAttemptMs_ != 0 && nowMs - lastConnectAttemptMs_ < 1000) {
        return;
    }
    lastConnectAttemptMs_ = nowMs;
    sock_ = net::connectTcp(host_, port_);
    if (sock_.valid()) {
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

LogReplaySource::LogReplaySource(std::string path, DecodeBuffer* decode)
    : path_(std::move(path)), decode_(decode) {}

LogReplaySource::~LogReplaySource() {
    stop();
}

bool LogReplaySource::open(std::string& error) {
    if (path_.empty()) {
        error = "log_replay requires a path to a VSD1 .log file or segment directory";
        return false;
    }
    {
        std::string lower = path_;
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0) {
            error = "log_replay does not unzip archives; extract segments first";
            return false;
        }
    }
    const auto files = vsd1::listLogFiles(path_);
    if (files.empty()) {
        error = "no VSD1 .log files at path";
        return false;
    }
    cancelIndex_.store(false, std::memory_order_relaxed);
    ready_.store(false, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    indexError_.clear();
    std::fprintf(stdout, "[photon-backend] log_replay indexing %zu segment(s) (background)\n", files.size());
    std::fflush(stdout);
    if (indexThread_.joinable()) {
        indexThread_.join();
    }
    indexThread_ = std::thread([this] { indexWorker(); });
    return true;
}

void LogReplaySource::indexWorker() {
    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    auto archive = vsd1::indexPath(path_, &err, &cancelIndex_);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> lock(indexMu_);
    if (archive) {
        archive_ = std::move(*archive);
        cursor_.bind(archive_);
        applyPendingTimeline();
        ready_.store(true, std::memory_order_release);
        std::fprintf(stdout,
                     "[photon-backend] log_replay index ready files=%zu batches=%zu min=%llu max=%llu elapsed_ms=%lld\n",
                     archive_.files.size(), archive_.batches.size(),
                     static_cast<unsigned long long>(archive_.minTimeMs),
                     static_cast<unsigned long long>(archive_.maxTimeMs), static_cast<long long>(ms));
        std::fflush(stdout);
        return;
    }
    failed_.store(true, std::memory_order_release);
    indexError_ = err.empty() ? "index failed" : std::move(err);
    std::fprintf(stdout, "[photon-backend] log_replay index failed (%s) elapsed_ms=%lld\n", indexError_.c_str(),
                 static_cast<long long>(ms));
    std::fflush(stdout);
}

void LogReplaySource::start() {
    std::lock_guard<std::mutex> lock(indexMu_);
    running_ = true;
    lastTickMs_ = 0;
    pendingCan_.clear();
    if (ready_.load(std::memory_order_acquire)) {
        cursor_.bind(archive_);
        applyPendingTimeline();
    }
}

void LogReplaySource::stop() {
    running_ = false;
    cancelIndex_.store(true, std::memory_order_relaxed);
    if (indexThread_.joinable()) {
        indexThread_.join();
    }
    pendingCan_.clear();
}

void LogReplaySource::emitRange(vsd1::ReplayCursor::EmitRange range) {
    for (std::size_t i = range.begin; i < range.end; ++i) {
        std::string readError;
        auto batch = vsd1::readBatch(archive_, archive_.batches[i], &readError);
        if (!batch) {
            static int failures = 0;
            if (failures < 8) {
                std::fprintf(stdout, "[photon-backend] log_replay readBatch[%zu] failed (%s)\n", i,
                             readError.c_str());
                ++failures;
            }
            continue;
        }
        if (!loggedFirstEmit_) {
            loggedFirstEmit_ = true;
            std::fprintf(stdout, "[photon-backend] log_replay first emit batch=%zu packets=%u\n", i,
                         static_cast<unsigned>(batch->count));
        }
        for (std::uint16_t p = 0; p < batch->count; ++p) {
            proto::CanMonitorItem item{};
            item.canId = batch->packets[p].canId;
            item.dlc = batch->packets[p].dlc > 8 ? 8 : batch->packets[p].dlc;
            item.timestampMs = batch->timestamp;
            std::memcpy(item.data, batch->packets[p].data, 8);
            pendingCan_.push_back(item);
            if (decode_) {
                decode_->ingest(item);
            }
        }
    }
    if (pendingCan_.size() > 1024) {
        pendingCan_.erase(pendingCan_.begin(),
                          pendingCan_.begin() + static_cast<std::ptrdiff_t>(pendingCan_.size() - 1024));
    }
}

void LogReplaySource::tick(std::uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(indexMu_);
    if (!running_ || !ready_.load(std::memory_order_acquire) || archive_.empty()) {
        return;
    }
    std::uint64_t dt = 0;
    if (lastTickMs_ != 0 && nowMs >= lastTickMs_) {
        dt = nowMs - lastTickMs_;
    }
    lastTickMs_ = nowMs;
    emitRange(cursor_.advance(dt));
}

void LogReplaySource::applyPendingTimeline() {
    if (!havePendingTimeline_) {
        return;
    }
    havePendingTimeline_ = false;
    switch (pendingMode_) {
        case ctrl::TimelineMode::Pause:
            cursor_.setMode(vsd1::ReplayCursor::Mode::Pause);
            break;
        case ctrl::TimelineMode::Play:
            cursor_.setMode(vsd1::ReplayCursor::Mode::Play);
            break;
        case ctrl::TimelineMode::Live:
        case ctrl::TimelineMode::Unknown:
            break;
    }
    if (pendingCursorMs_ != 0) {
        cursor_.seek(pendingCursorMs_);
        lastTickMs_ = 0;
    }
    cursor_.setSpeed(pendingSpeed_);
}

void LogReplaySource::controlTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs, double speed) {
    std::lock_guard<std::mutex> lock(indexMu_);
    if (!ready_.load(std::memory_order_acquire)) {
        havePendingTimeline_ = true;
        pendingMode_ = mode;
        pendingCursorMs_ = cursorMs;
        pendingSpeed_ = speed;
        return;
    }
    switch (mode) {
        case ctrl::TimelineMode::Pause:
            cursor_.setMode(vsd1::ReplayCursor::Mode::Pause);
            break;
        case ctrl::TimelineMode::Play:
            cursor_.setMode(vsd1::ReplayCursor::Mode::Play);
            break;
        case ctrl::TimelineMode::Live:
            break;
        case ctrl::TimelineMode::Unknown:
            break;
    }
    if (cursorMs != 0) {
        cursor_.seek(cursorMs);
        lastTickMs_ = 0;
    }
    cursor_.setSpeed(speed);
}

std::vector<proto::CanMonitorItem> LogReplaySource::takeCanFrames() {
    std::lock_guard<std::mutex> lock(indexMu_);
    auto out = std::move(pendingCan_);
    pendingCan_.clear();
    return out;
}

proto::TimelineStatus LogReplaySource::timeline() const {
    proto::TimelineStatus status{};
    std::lock_guard<std::mutex> lock(indexMu_);
    if (!ready_.load(std::memory_order_acquire)) {
        status.mode = static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
        return status;
    }
    switch (cursor_.mode()) {
        case vsd1::ReplayCursor::Mode::Pause:
            status.mode = static_cast<std::uint8_t>(proto::TimelineMode::Pause);
            break;
        case vsd1::ReplayCursor::Mode::Play:
            status.mode = static_cast<std::uint8_t>(proto::TimelineMode::Play);
            break;
        case vsd1::ReplayCursor::Mode::Live:
            status.mode = static_cast<std::uint8_t>(proto::TimelineMode::Live);
            break;
    }
    status.cursorMs = cursor_.cursorMs();
    status.minTimeMs = archive_.minTimeMs;
    status.maxTimeMs = archive_.maxTimeMs;
    status.totalBatches = archive_.batches.size();
    return status;
}

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

SourceHub::SourceHub(std::string daqHost, std::uint16_t daqPort) : daqHost_(std::move(daqHost)), daqPort_(daqPort) {}

bool SourceHub::select(ctrl::SourceKind kind, const std::string& host, std::uint16_t port, const std::string& path,
                       std::string& error, const pcan::Config& pcanCfg) {
    stop();
    decode_.clear();
    active_ = {};
    switch (kind) {
        case ctrl::SourceKind::Synthetic:
            source_ = std::make_unique<SyntheticSource>();
            source_->start();
            active_.kind = ctrl::SourceKind::Synthetic;
            std::fprintf(stdout, "[photon-backend] source = synthetic\n");
            return true;
        case ctrl::SourceKind::DaqTcp: {
            const std::string useHost = host.empty() ? daqHost_ : host;
            const std::uint16_t usePort = port == 0 ? daqPort_ : port;
            source_ = std::make_unique<DaqTcpSource>(useHost, usePort, &decode_);
            source_->start();
            active_.kind = ctrl::SourceKind::DaqTcp;
            active_.host = useHost;
            active_.port = usePort;
            std::fprintf(stdout, "[photon-backend] source = daq_tcp %s:%u\n", useHost.c_str(),
                         static_cast<unsigned>(usePort));
            return true;
        }
        case ctrl::SourceKind::Pcan: {
            auto pcanSrc = std::make_unique<PcanSource>(pcanCfg, &decode_);
            if (!pcanSrc->open(error)) {
                return false;
            }
            source_ = std::move(pcanSrc);
            source_->start();
            active_.kind = ctrl::SourceKind::Pcan;
            std::fprintf(stdout, "[photon-backend] source = pcan channel=%d baud=%d listen_only=%s\n",
                         pcanCfg.channel, pcanCfg.baud, pcanCfg.listenOnly ? "true" : "false");
            return true;
        }
        case ctrl::SourceKind::LogReplay: {
            auto replay = std::make_unique<LogReplaySource>(path, &decode_);
            if (!replay->open(error)) {
                return false;
            }
            source_ = std::move(replay);
            source_->start();
            active_.kind = ctrl::SourceKind::LogReplay;
            active_.path = path;
            std::fprintf(stdout, "[photon-backend] source = log_replay %s\n", path.c_str());
            return true;
        }
        case ctrl::SourceKind::None:
            error = "no source selected";
            return false;
    }
    error = "unknown source";
    return false;
}

void SourceHub::tick(std::uint64_t nowMs) {
    if (source_) {
        source_->tick(nowMs);
    }
}

void SourceHub::attachPoll(std::vector<net::PollFd>& fds) const {
    if (source_) {
        source_->attachPoll(fds);
    }
}

void SourceHub::onPoll(const std::vector<net::PollFd>& fds) {
    if (source_) {
        source_->onPoll(fds);
    }
}

std::vector<proto::TelemetryBlock> SourceHub::takeTelemetry(const std::unordered_set<std::uint32_t>& ids) {
    std::vector<proto::TelemetryBlock> blocks;
    if (source_) {
        blocks = source_->takeTelemetry(ids);
    }
    auto decoded = decode_.takeTelemetry(ids);
    blocks.insert(blocks.end(), decoded.begin(), decoded.end());
    return blocks;
}

void SourceHub::setDatabase(const dbc::Database* db) {
    decode_.setDatabase(db);
}

void SourceHub::controlTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs, double speed) {
    if (source_) {
        source_->controlTimeline(mode, cursorMs, speed);
    }
}

std::vector<proto::CanMonitorItem> SourceHub::takeCanFrames() {
    if (!source_) {
        return {};
    }
    return source_->takeCanFrames();
}

proto::TimelineStatus SourceHub::timeline() const {
    if (!source_) {
        return proto::TimelineStatus{};
    }
    return source_->timeline();
}

ctrl::SourceKind SourceHub::kind() const {
    return source_ ? source_->kind() : ctrl::SourceKind::None;
}

bool SourceHub::listenOnly() const {
    return source_ ? source_->listenOnly() : true;
}

bool SourceHub::writeCan(const dbc::EncodedFrame& frame, std::string& error) {
    if (!source_ || source_->kind() != ctrl::SourceKind::Pcan) {
        error = "source is not pcan";
        return false;
    }
    return source_->writeCan(frame.canId, frame.dlc, frame.data, frame.extended, error);
}

void SourceHub::stop() {
    if (source_) {
        source_->stop();
        source_.reset();
    }
}

}  // namespace photon
