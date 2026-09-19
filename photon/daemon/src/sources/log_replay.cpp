#include "photon/source.hpp"

#include "photon/log_sessions.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>

namespace photon {
namespace {

constexpr std::uint64_t kPrimeLookbackMs = 180000;
// First snapshot: enough for a BPS mux cycle (32 taps) without decoding 180 s.
constexpr std::uint64_t kFastPrimeLookbackMs = 12000;
constexpr auto kPrimeSettle = std::chrono::milliseconds(40);

}  // namespace

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

}  // namespace photon
