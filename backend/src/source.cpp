#include "photon/source.hpp"

#include "photon/log_sessions.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <utility>

namespace photon {
namespace {

constexpr std::size_t kRingCap = 16384;
constexpr std::uint64_t kPrimeLookbackMs = 180000;
// First snapshot: enough for a BPS mux cycle (32 taps) without decoding 180 s.
constexpr std::uint64_t kFastPrimeLookbackMs = 12000;
// Live subscribe: default plot window (15 s) + mux cycle. The full 16 k ring
// used to dump minutes of history, so a second window walked old values.
constexpr std::uint64_t kLiveSnapshotLookbackMs = 15000;
constexpr std::size_t kLiveSnapshotMaxSamples = 2000;
constexpr auto kPrimeSettle = std::chrono::milliseconds(40);
constexpr std::uint64_t kGenPeriodMs = 10;  // 100 Hz

double nowSeconds(std::uint64_t nowMs, std::uint64_t startMs) {
    return static_cast<double>(nowMs - startMs) / 1000.0;
}

void fillSnapshotBlock(const SignalRing& ring, proto::TelemetryBlock& block) {
    if (ring.samples.empty()) {
        return;
    }
    const double newest = ring.samples.back().timestampMs;
    const double floorTs = newest > static_cast<double>(kLiveSnapshotLookbackMs)
                               ? newest - static_cast<double>(kLiveSnapshotLookbackMs)
                               : 0.0;
    auto begin = ring.samples.begin();
    if (floorTs > 0.0) {
        begin = std::lower_bound(ring.samples.begin(), ring.samples.end(), floorTs,
                                 [](const SignalSample& sample, double ts) { return sample.timestampMs < ts; });
    }
    std::size_t start = static_cast<std::size_t>(begin - ring.samples.begin());
    if (ring.samples.size() - start > kLiveSnapshotMaxSamples) {
        start = ring.samples.size() - kLiveSnapshotMaxSamples;
    }
    block.timestamps.reserve(ring.samples.size() - start);
    block.values.reserve(ring.samples.size() - start);
    for (std::size_t i = start; i < ring.samples.size(); ++i) {
        block.timestamps.push_back(ring.samples[i].timestampMs);
        block.values.push_back(ring.samples[i].value);
    }
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

void DecodeBuffer::setWanted(std::unordered_set<std::uint32_t> ids) {
    restrictWanted_ = true;
    wanted_ = std::move(ids);
    for (auto it = rings_.begin(); it != rings_.end();) {
        if (!wanted_.contains(it->first)) {
            it = rings_.erase(it);
        } else {
            ++it;
        }
    }
}

void DecodeBuffer::markCaughtUp() {
    for (auto& [id, ring] : rings_) {
        (void)id;
        ring.lastEmitted = ring.samples.size();
    }
}

const SignalRing* DecodeBuffer::ring(std::uint32_t id) const {
    const auto it = rings_.find(id);
    if (it == rings_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool DecodeBuffer::latest(std::uint32_t id, double& value) const {
    const SignalRing* r = ring(id);
    if (r == nullptr || r->samples.empty()) {
        return false;
    }
    value = r->samples.back().value;
    return true;
}

void DecodeBuffer::ingest(const proto::CanMonitorItem& item) {
    if (db_ == nullptr) {
        return;
    }
    if (restrictWanted_ && wanted_.empty()) {
        return;
    }
    const dbc::Message* msg = db_->findMessage(item.canId);
    if (msg == nullptr) {
        return;
    }
    for (const auto& sig : msg->signals) {
        if (restrictWanted_ && !wanted_.contains(sig.signalId)) {
            continue;
        }
        double value = 0.0;
        if (!dbc::decodeSignal(sig, item.data, item.dlc, value)) {
            continue;
        }
        SignalRing& ring = rings_[sig.signalId];
        ring.id = sig.signalId;
        ring.canId = item.canId;
        ring.samples.push_back(SignalSample{static_cast<double>(item.timestampMs), value});
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

std::vector<proto::TelemetryBlock> DecodeBuffer::snapshot(const std::unordered_set<std::uint32_t>& ids) const {
    std::vector<proto::TelemetryBlock> blocks;
    for (const auto id : ids) {
        const SignalRing* r = ring(id);
        if (r == nullptr || r->samples.empty()) {
            continue;
        }
        proto::TelemetryBlock block;
        block.signalId = id;
        fillSnapshotBlock(*r, block);
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

std::vector<proto::TelemetryBlock> SyntheticSource::snapshot(const std::unordered_set<std::uint32_t>& ids) const {
    std::vector<proto::TelemetryBlock> blocks;
    for (const auto& ring : signals_) {
        if (!ids.contains(ring.id) || ring.samples.empty()) {
            continue;
        }
        proto::TelemetryBlock block;
        block.signalId = ring.id;
        fillSnapshotBlock(ring, block);
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

LogReplaySource::LogReplaySource(std::string path, DecodeBuffer* decode)
    : path_(std::move(path)), decode_(decode) {}

LogReplaySource::~LogReplaySource() {
    stop();
}

bool LogReplaySource::open(std::string& error) {
    path_ = logsess::resolveReplayPath(path_);
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
    failed_.store(false, std::memory_order_relaxed);
    indexError_.clear();
    if (auto cached = vsd1::tryLoadIndexCache(files)) {
        std::lock_guard<std::mutex> lock(indexMu_);
        archive_ = std::move(*cached);
        archive_.complete = true;
        ready_.store(true, std::memory_order_release);
        indexDone_.store(true, std::memory_order_release);
        std::fprintf(stdout, "[photon-backend] log_replay index cache files=%zu batches=%zu\n",
                     archive_.files.size(), archive_.batches.size());
        std::fflush(stdout);
        return true;
    }
    ready_.store(false, std::memory_order_relaxed);
    indexDone_.store(false, std::memory_order_relaxed);
    std::fprintf(stdout, "[photon-backend] log_replay indexing %zu segment(s) (background)\n", files.size());
    std::fflush(stdout);
    if (indexThread_.joinable()) {
        indexThread_.join();
    }
    indexThread_ = std::thread([this] { indexWorker(); });
    return true;
}

void LogReplaySource::publishPartialIndex() {
    if (archive_.batches.empty()) {
        return;
    }
    archive_.minTimeMs = archive_.batches.front().timestampMs;
    archive_.maxTimeMs = archive_.batches.back().timestampMs;
    archive_.complete = false;
    if (ready_.load(std::memory_order_relaxed)) {
        return;
    }
    cursor_.bind(archive_);
    applyPendingTimeline();
    emitFastThenScheduleFull();
    ready_.store(true, std::memory_order_release);
    std::fprintf(stdout, "[photon-backend] log_replay playing while indexing batches=%zu\n",
                 archive_.batches.size());
    std::fflush(stdout);
}

void LogReplaySource::indexWorker() {
    const auto t0 = std::chrono::steady_clock::now();
    const auto files = vsd1::listLogFiles(path_);
    if (auto cached = vsd1::tryLoadIndexCache(files)) {
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lock(indexMu_);
        archive_ = std::move(*cached);
        archive_.complete = true;
        cursor_.bind(archive_);
        applyPendingTimeline();
        emitFastThenScheduleFull();
        ready_.store(true, std::memory_order_release);
        indexDone_.store(true, std::memory_order_release);
        std::fprintf(stdout,
                     "[photon-backend] log_replay index cache files=%zu batches=%zu elapsed_ms=%lld\n",
                     archive_.files.size(), archive_.batches.size(), static_cast<long long>(ms));
        std::fflush(stdout);
        return;
    }

    std::uint64_t bytes = 0;
    for (const auto& file : files) {
        std::error_code ec;
        bytes += std::filesystem::file_size(file, ec);
    }
    {
        std::lock_guard<std::mutex> lock(indexMu_);
        archive_ = {};
        archive_.files = files;
        archive_.complete = false;
        if (bytes > 0) {
            archive_.batches.reserve(static_cast<std::size_t>(bytes / 1500 + 8));
        }
    }

    constexpr std::size_t kFlush = 4096;
    std::vector<vsd1::IndexedBatch> pending;
    pending.reserve(kFlush);
    auto flush = [&]() {
        if (pending.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(indexMu_);
        archive_.batches.insert(archive_.batches.end(), std::make_move_iterator(pending.begin()),
                                std::make_move_iterator(pending.end()));
        pending.clear();
        publishPartialIndex();
    };

    std::string err;
    const bool ok = vsd1::walkLogFiles(files, &cancelIndex_, &err, [&](vsd1::IndexedBatch&& item) {
        pending.push_back(std::move(item));
        if (pending.size() >= kFlush) {
            flush();
        }
    });
    flush();

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard<std::mutex> lock(indexMu_);
        if (!ok) {
            failed_.store(true, std::memory_order_release);
            indexError_ = err.empty() ? "index failed" : std::move(err);
            std::fprintf(stdout, "[photon-backend] log_replay index failed (%s) elapsed_ms=%lld\n",
                         indexError_.c_str(), static_cast<long long>(ms));
            std::fflush(stdout);
            return;
        }
        if (archive_.batches.empty()) {
            failed_.store(true, std::memory_order_release);
            indexError_ = "segments have no readable VSD1/CANP frames";
            return;
        }
        const auto cursorMs = cursor_.cursorMs();
        vsd1::finishArchive(archive_);
        archive_.complete = true;
        if (ready_.load(std::memory_order_relaxed)) {
            cursor_.seek(cursorMs == 0 ? archive_.minTimeMs : cursorMs);
            emitAroundCursor();
            snapshotDue_.store(true, std::memory_order_release);
        } else {
            cursor_.bind(archive_);
            applyPendingTimeline();
            emitAroundCursor();
            ready_.store(true, std::memory_order_release);
            snapshotDue_.store(true, std::memory_order_release);
        }
        indexDone_.store(true, std::memory_order_release);
        std::fprintf(stdout,
                     "[photon-backend] log_replay index ready files=%zu batches=%zu min=%llu max=%llu elapsed_ms=%lld\n",
                     archive_.files.size(), archive_.batches.size(),
                     static_cast<unsigned long long>(archive_.minTimeMs),
                     static_cast<unsigned long long>(archive_.maxTimeMs), static_cast<long long>(ms));
        std::fflush(stdout);
    }
    vsd1::saveIndexCache(archive_);
}

void LogReplaySource::start() {
    std::lock_guard<std::mutex> lock(indexMu_);
    running_ = true;
    lastTickMs_ = 0;
    pendingCan_.clear();
    if (ready_.load(std::memory_order_acquire)) {
        cursor_.bind(archive_);
        applyPendingTimeline();
        emitFastThenScheduleFull();
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

void LogReplaySource::emitRange(ReplayView& view, vsd1::ReplayCursor::EmitRange range) {
    for (std::size_t i = range.begin; i < range.end; ++i) {
        std::string readError;
        auto batch = vsd1::readBatch(archive_, archive_.batches[i], &readError);
        if (!batch) {
            continue;
        }
        for (std::uint16_t p = 0; p < batch->count; ++p) {
            proto::CanMonitorItem item{};
            item.canId = batch->packets[p].canId;
            item.dlc = batch->packets[p].dlc > 8 ? 8 : batch->packets[p].dlc;
            item.timestampMs = batch->timestamp;
            std::memcpy(item.data, batch->packets[p].data, 8);
            view.pendingCan.push_back(item);
            view.decode.ingest(item);
        }
    }
    if (view.pendingCan.size() > 1024) {
        view.pendingCan.erase(view.pendingCan.begin(),
                              view.pendingCan.begin() + static_cast<std::ptrdiff_t>(view.pendingCan.size() - 1024));
    }
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
    flushPrimeIfDue();
    emitRange(cursor_.advance(dt));
}

void LogReplaySource::schedulePrime(bool immediate) {
    lastSeekAt_ = std::chrono::steady_clock::now();
    if (immediate) {
        emitFastThenScheduleFull();
        return;
    }
    primeDue_ = true;
}

void LogReplaySource::flushPrimeIfDue() {
    if (!primeDue_) {
        return;
    }
    if (std::chrono::steady_clock::now() - lastSeekAt_ < kPrimeSettle) {
        return;
    }
    emitAroundCursor();
    snapshotDue_.store(true, std::memory_order_release);
    primeDue_ = false;
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
        if (pendingMode_ == ctrl::TimelineMode::Play || pendingMode_ == ctrl::TimelineMode::Live) {
            schedulePrime(true);
        }
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
            cursor_.setMode(vsd1::ReplayCursor::Mode::Play);
            if (!archive_.empty()) {
                cursor_.seek(archive_.maxTimeMs);
            }
            lastTickMs_ = 0;
            schedulePrime(true);
            cursor_.setSpeed(speed);
            return;
        case ctrl::TimelineMode::Unknown:
            break;
    }
    if (cursorMs != 0) {
        cursor_.seek(cursorMs);
        lastTickMs_ = 0;
        if (mode == ctrl::TimelineMode::Play) {
            schedulePrime(true);
        }
    }
    cursor_.setSpeed(speed);
}

bool LogReplaySource::consumeSnapshotDue() {
    return snapshotDue_.exchange(false, std::memory_order_acq_rel);
}

bool LogReplaySource::replayReady() const {
    return ready_.load(std::memory_order_acquire);
}

void LogReplaySource::resetReplay(ReplayView& view) {
    view = ReplayView{};
}

void LogReplaySource::applyPendingTimeline(ReplayView& view) {
    if (!view.havePending) {
        return;
    }
    view.havePending = false;
    switch (view.pendingMode) {
        case ctrl::TimelineMode::Pause:
            view.cursor.setMode(vsd1::ReplayCursor::Mode::Pause);
            break;
        case ctrl::TimelineMode::Play:
            view.cursor.setMode(vsd1::ReplayCursor::Mode::Play);
            break;
        case ctrl::TimelineMode::Live:
            view = ReplayView{};
            return;
        case ctrl::TimelineMode::Unknown:
            break;
    }
    if (view.pendingCursorMs != 0) {
        view.cursor.seek(view.pendingCursorMs);
        view.lastTickMs = 0;
        if (view.pendingMode == ctrl::TimelineMode::Play) {
            schedulePrime(view, true);
        }
    }
    view.cursor.setSpeed(view.pendingSpeed);
}

void LogReplaySource::schedulePrime(ReplayView& view, bool immediate) {
    view.lastSeekAt = std::chrono::steady_clock::now();
    if (immediate) {
        emitAroundCursor(view, kFastPrimeLookbackMs);
        view.snapshotDue = true;
        view.primeDue = true;
        return;
    }
    view.primeDue = true;
}

void LogReplaySource::flushPrimeIfDue(ReplayView& view) {
    if (!view.primeDue) {
        return;
    }
    if (std::chrono::steady_clock::now() - view.lastSeekAt < kPrimeSettle) {
        return;
    }
    // Per-client log pointer: keep the WASM tab on a 12 s window. A 180 s
    // decode dump after Live→log was aborting the public UI.
    emitAroundCursor(view, kFastPrimeLookbackMs);
    view.snapshotDue = true;
    view.primeDue = false;
}

void LogReplaySource::emitAroundCursor(ReplayView& view, std::uint64_t lookbackMs) {
    if (archive_.empty()) {
        return;
    }
    const std::uint64_t lookback = lookbackMs == 0 ? kPrimeLookbackMs : lookbackMs;
    const std::uint64_t atMs = view.cursor.cursorMs();
    const std::uint64_t fromMs = atMs > lookback ? atMs - lookback : archive_.minTimeMs;
    const std::size_t begin = vsd1::seekIndex(archive_, fromMs);
    const std::size_t at = vsd1::seekIndex(archive_, atMs);
    const std::size_t end = std::min(archive_.batches.size(), at + 1);
    view.decode.clear();
    emitRange(view, {begin, end});
    view.decode.markCaughtUp();
}

void LogReplaySource::attachReplay(ReplayView& view) {
    std::lock_guard<std::mutex> lock(indexMu_);
    if (!ready_.load(std::memory_order_acquire) || archive_.empty()) {
        return;
    }
    if (view.bound) {
        return;
    }
    view.cursor.bind(archive_);
    view.bound = true;
    view.lastTickMs = 0;
    view.pendingCan.clear();
    // bind() defaults to Play. Opening a day must not start streaming the
    // archive until the client sends Play — that dump aborts the WASM tab.
    if (!view.havePending) {
        view.cursor.setMode(vsd1::ReplayCursor::Mode::Pause);
    }
    applyPendingTimeline(view);
    emitAroundCursor(view, kFastPrimeLookbackMs);
    view.snapshotDue = true;
    view.primeDue = true;
    view.lastSeekAt = std::chrono::steady_clock::now();
}

void LogReplaySource::tickReplay(ReplayView& view, std::uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(indexMu_);
    if (!running_ || !ready_.load(std::memory_order_acquire) || archive_.empty()) {
        return;
    }
    if (!view.bound) {
        view.cursor.bind(archive_);
        view.bound = true;
        if (!view.havePending) {
            view.cursor.setMode(vsd1::ReplayCursor::Mode::Pause);
        }
        applyPendingTimeline(view);
        emitAroundCursor(view, kFastPrimeLookbackMs);
        view.snapshotDue = true;
        view.primeDue = true;
        view.lastSeekAt = std::chrono::steady_clock::now();
        view.lastTickMs = nowMs;
        return;
    }
    std::uint64_t dt = 0;
    if (view.lastTickMs != 0 && nowMs >= view.lastTickMs) {
        dt = nowMs - view.lastTickMs;
    }
    view.lastTickMs = nowMs;
    flushPrimeIfDue(view);
    emitRange(view, view.cursor.advance(dt));
}

void LogReplaySource::controlReplay(ReplayView& view, ctrl::TimelineMode mode, std::uint64_t cursorMs, double speed) {
    std::lock_guard<std::mutex> lock(indexMu_);
    if (!ready_.load(std::memory_order_acquire)) {
        view.havePending = true;
        view.pendingMode = mode;
        view.pendingCursorMs = cursorMs;
        view.pendingSpeed = speed;
        return;
    }
    if (!view.bound) {
        if (!archive_.empty()) {
            view.cursor.bind(archive_);
            view.bound = true;
        }
    }
    switch (mode) {
        case ctrl::TimelineMode::Pause:
            view.cursor.setMode(vsd1::ReplayCursor::Mode::Pause);
            break;
        case ctrl::TimelineMode::Play:
            view.cursor.setMode(vsd1::ReplayCursor::Mode::Play);
            break;
        case ctrl::TimelineMode::Live:
            view = ReplayView{};
            return;
        case ctrl::TimelineMode::Unknown:
            break;
    }
    if (cursorMs != 0) {
        view.cursor.seek(cursorMs);
        view.lastTickMs = 0;
        if (mode == ctrl::TimelineMode::Play) {
            schedulePrime(view, true);
        }
    } else if (mode == ctrl::TimelineMode::Play && view.bound && !archive_.empty() &&
               view.cursor.cursorMs() == 0) {
        view.cursor.seek(archive_.minTimeMs);
        view.lastTickMs = 0;
        schedulePrime(view, true);
    }
    view.cursor.setSpeed(speed);
}

proto::TimelineStatus LogReplaySource::replayTimeline(const ReplayView& view) const {
    proto::TimelineStatus status{};
    std::lock_guard<std::mutex> lock(indexMu_);
    if (!ready_.load(std::memory_order_acquire)) {
        status.mode = static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
        return status;
    }
    const vsd1::ReplayCursor& cursor = view.bound ? view.cursor : cursor_;
    switch (cursor.mode()) {
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
    status.cursorMs = cursor.cursorMs();
    status.minTimeMs = archive_.minTimeMs;
    status.maxTimeMs = archive_.maxTimeMs;
    status.totalBatches = archive_.batches.size();
    return status;
}

std::vector<proto::CanMonitorItem> LogReplaySource::takeReplayCan(ReplayView& view) {
    auto out = std::move(view.pendingCan);
    view.pendingCan.clear();
    return out;
}

std::vector<proto::TelemetryBlock> LogReplaySource::takeReplayTelemetry(
    ReplayView& view, const std::unordered_set<std::uint32_t>& ids) {
    return view.decode.takeTelemetry(ids);
}

std::vector<proto::TelemetryBlock> LogReplaySource::snapshotReplay(
    const ReplayView& view, const std::unordered_set<std::uint32_t>& ids) const {
    return view.decode.snapshot(ids);
}

void LogReplaySource::emitAroundCursor(std::uint64_t lookbackMs) {
    if (archive_.empty()) {
        return;
    }
    const std::uint64_t lookback = lookbackMs == 0 ? kPrimeLookbackMs : lookbackMs;
    const std::uint64_t atMs = cursor_.cursorMs();
    const std::uint64_t fromMs = atMs > lookback ? atMs - lookback : archive_.minTimeMs;
    const std::size_t begin = vsd1::seekIndex(archive_, fromMs);
    const std::size_t at = vsd1::seekIndex(archive_, atMs);
    const std::size_t end = std::min(archive_.batches.size(), at + 1);
    if (decode_ != nullptr) {
        decode_->clear();
    }
    emitRange({begin, end});
    if (decode_ != nullptr) {
        decode_->markCaughtUp();
    }
    std::fprintf(stdout, "[photon-backend] log_replay primed decode batches=%zu..%zu cursor=%llu lookback_ms=%llu\n",
                 begin, end, static_cast<unsigned long long>(cursor_.cursorMs()),
                 static_cast<unsigned long long>(lookback));
}

void LogReplaySource::emitFastThenScheduleFull() {
    emitAroundCursor(kFastPrimeLookbackMs);
    snapshotDue_.store(true, std::memory_order_release);
    primeDue_ = true;
    lastSeekAt_ = std::chrono::steady_clock::now();
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
    if (kind == ctrl::SourceKind::LogReplay) {
        const std::string resolved = logsess::resolveReplayPath(path);
        if (active_.kind == ctrl::SourceKind::LogReplay && !resolved.empty() && active_.path == resolved &&
            source_ != nullptr) {
            std::fprintf(stdout, "[photon-backend] source = log_replay (already) %s\n", resolved.c_str());
            return true;
        }
    }
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
            source_ = std::make_unique<DaqTcpSource>(useHost, usePort, &decode_, password_);
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
            const std::string resolved = logsess::resolveReplayPath(path);
            auto replay = std::make_unique<LogReplaySource>(resolved, &decode_);
            if (!replay->open(error)) {
                return false;
            }
            source_ = std::move(replay);
            source_->start();
            active_.kind = ctrl::SourceKind::LogReplay;
            active_.path = resolved;
            std::fprintf(stdout, "[photon-backend] source = log_replay %s\n", resolved.c_str());
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

bool SourceHub::openLog(const std::string& path, std::string& error) {
    const std::string resolved = logsess::resolveReplayPath(path);
    if (resolved.empty()) {
        error = "log_replay requires a path to a VSD1 .log file or segment directory";
        return false;
    }
    if (log_ && logPath_ == resolved) {
        return true;
    }
    if (source_ && source_->kind() == ctrl::SourceKind::LogReplay && active_.path == resolved) {
        // Drop the extra pointer so replaySource() uses source_ again. Leaving
        // log_ around after a switch back to the hub path keeps clients on the
        // previous archive.
        if (log_) {
            log_->stop();
            log_.reset();
        }
        logPath_ = resolved;
        return true;
    }
    auto replay = std::make_unique<LogReplaySource>(resolved, nullptr);
    if (!replay->open(error)) {
        return false;
    }
    replay->start();
    log_ = std::move(replay);
    logPath_ = resolved;
    std::fprintf(stdout, "[photon-backend] log pointer %s (live source stays %s)\n", logPath_.c_str(),
                 std::string(ctrl::sourceName(active_.kind)).c_str());
    return true;
}

ISource* SourceHub::replaySource() {
    if (log_) {
        return log_.get();
    }
    if (source_ && source_->kind() == ctrl::SourceKind::LogReplay) {
        return source_.get();
    }
    return nullptr;
}

const ISource* SourceHub::replaySource() const {
    if (log_) {
        return log_.get();
    }
    if (source_ && source_->kind() == ctrl::SourceKind::LogReplay) {
        return source_.get();
    }
    return nullptr;
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

std::vector<proto::TelemetryBlock> SourceHub::snapshotTelemetry(const std::unordered_set<std::uint32_t>& ids) const {
    std::vector<proto::TelemetryBlock> blocks;
    if (source_) {
        blocks = source_->snapshot(ids);
    }
    auto decoded = decode_.snapshot(ids);
    blocks.insert(blocks.end(), decoded.begin(), decoded.end());
    return blocks;
}

bool SourceHub::consumeSnapshotDue() {
    return source_ != nullptr && source_->consumeSnapshotDue();
}

void SourceHub::setDatabase(const dbc::Database* db) {
    decode_.setDatabase(db);
}

void SourceHub::setDecodeWanted(std::unordered_set<std::uint32_t> ids) {
    decode_.setWanted(std::move(ids));
}

bool SourceHub::latestSignal(std::uint32_t id, double& value) const {
    return decode_.latest(id, value);
}

void SourceHub::controlTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs, double speed) {
    if (source_) {
        source_->controlTimeline(mode, cursorMs, speed);
    }
}

bool SourceHub::replayActive() const {
    return replaySource() != nullptr;
}

bool SourceHub::hasLog() const {
    return replaySource() != nullptr;
}

void SourceHub::attachReplay(ReplayView& view) {
    if (auto* replay = replaySource()) {
        replay->attachReplay(view);
    }
}

void SourceHub::resetReplay(ReplayView& view) {
    if (auto* replay = replaySource()) {
        replay->resetReplay(view);
    } else {
        view = ReplayView{};
    }
}

void SourceHub::tickReplay(ReplayView& view, std::uint64_t nowMs) {
    if (auto* replay = replaySource()) {
        replay->tickReplay(view, nowMs);
    }
}

void SourceHub::controlReplay(ReplayView& view, ctrl::TimelineMode mode, std::uint64_t cursorMs, double speed) {
    if (auto* replay = replaySource()) {
        replay->controlReplay(view, mode, cursorMs, speed);
    }
}

proto::TimelineStatus SourceHub::replayTimeline(const ReplayView& view) const {
    if (const auto* replay = replaySource()) {
        return replay->replayTimeline(view);
    }
    return proto::TimelineStatus{};
}

std::vector<proto::CanMonitorItem> SourceHub::takeReplayCan(ReplayView& view) {
    if (auto* replay = replaySource()) {
        return replay->takeReplayCan(view);
    }
    return {};
}

std::vector<proto::TelemetryBlock> SourceHub::takeReplayTelemetry(ReplayView& view,
                                                                  const std::unordered_set<std::uint32_t>& ids) {
    if (auto* replay = replaySource()) {
        return replay->takeReplayTelemetry(view, ids);
    }
    return {};
}

std::vector<proto::TelemetryBlock> SourceHub::snapshotReplay(const ReplayView& view,
                                                             const std::unordered_set<std::uint32_t>& ids) const {
    if (const auto* replay = replaySource()) {
        return replay->snapshotReplay(view, ids);
    }
    return {};
}

bool SourceHub::consumeReplaySnapshotDue(ReplayView& view) {
    if (auto* replay = replaySource()) {
        return replay->consumeReplaySnapshotDue(view);
    }
    return false;
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
    if (log_) {
        log_->stop();
        log_.reset();
        logPath_.clear();
    }
    if (source_) {
        source_->stop();
        source_.reset();
    }
}

tracksim::MeasuredDrive SourceHub::measuredDrive(const dbc::Database* db) const {
    tracksim::MeasuredDrive out;
    if (db == nullptr) {
        return out;
    }

    auto findId = [&](const char* message, const char* name) -> std::uint32_t {
        const dbc::Message* msg = db->findMessageByName(message);
        if (msg == nullptr) {
            return 0;
        }
        for (const auto& sig : msg->signals) {
            if (sig.name == name) {
                return sig.signalId;
            }
        }
        return 0;
    };

    auto stats = [&](std::uint32_t id, double& mean, double& mx, double& t0, double& t1, int& n) -> bool {
        const SignalRing* ring = decode_.ring(id);
        if (ring == nullptr || ring->samples.empty()) {
            return false;
        }
        double sum = 0.0;
        mx = ring->samples.front().value;
        t0 = ring->samples.front().timestampMs;
        t1 = t0;
        for (const auto& s : ring->samples) {
            sum += s.value;
            if (s.value > mx) {
                mx = s.value;
            }
            if (s.timestampMs < t0) {
                t0 = s.timestampMs;
            }
            if (s.timestampMs > t1) {
                t1 = s.timestampMs;
            }
        }
        n = static_cast<int>(ring->samples.size());
        mean = sum / static_cast<double>(n);
        return true;
    };

    double t0 = 0.0;
    double t1 = 0.0;
    int nSpeed = 0;
    int nPackV = 0;
    int nPackI = 0;
    double dummy = 0.0;
    if (stats(findId("MC_VelocityMeasurement", "MC_VehicleVelocity"), out.speedMpsMean, out.speedMpsMax, t0,
              t1, nSpeed)) {
        out.haveSpeed = true;
        out.samples = nSpeed;
    }
    double packT0 = t0;
    double packT1 = t1;
    if (stats(findId("BPS_Status", "Main_Battery_Voltage"), out.packVMean, dummy, packT0, packT1, nPackV)) {
        out.havePackV = true;
        out.socEst = tracksim::estimatePackSoc(out.packVMean);
        if (nPackV > out.samples) {
            out.samples = nPackV;
        }
        if (packT1 - packT0 > t1 - t0) {
            t0 = packT0;
            t1 = packT1;
        }
    }
    if (stats(findId("MC_BusMeasurement", "MC_BusCurrent"), out.packIMean, dummy, dummy, dummy, nPackI) &&
        out.havePackV) {
        out.electricalWMean = out.packVMean * out.packIMean;
    }

    double arrayW = 0.0;
    int arrayPairs = 0;
    const char* mppt[][3] = {
        {"MPPT_A_PowerMeasurements", "MPPT_Input_Voltage", "MPPT_Input_Current"},
        {"MPPT_B_PowerMeasurements", "MPPT_Input_Voltage", "MPPT_Input_Current"},
        {"MPPT_C_PowerMeasurements", "MPPT_Input_Voltage", "MPPT_Input_Current"},
    };
    for (const auto& row : mppt) {
        double vin = 0.0;
        double iin = 0.0;
        int nv = 0;
        int ni = 0;
        const bool haveV = stats(findId(row[0], row[1]), vin, dummy, dummy, dummy, nv);
        const bool haveI = stats(findId(row[0], row[2]), iin, dummy, dummy, dummy, ni);
        if (haveV && haveI) {
            arrayW += vin * iin;
            ++arrayPairs;
        }
    }
    if (arrayPairs > 0) {
        out.haveArray = true;
        out.arrayWMean = arrayW;
    }

    if (t1 > t0) {
        out.windowS = (t1 - t0) / 1000.0;
    }
    out.have = out.haveSpeed || out.havePackV || out.haveArray;
    tracksim::fitCarFromMeasured(out);
    return out;
}

}  // namespace photon
