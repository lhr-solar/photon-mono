#include "photon_ui/arena.hpp"
#include "photon_ui/page_url.hpp"

#include "photon/interp.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace photon::ui {
namespace {

std::uint64_t wallMs() {
    using clock = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

}  // namespace

void SignalRing::push(double ts, double value) {
    if (t.size() < cap) {
        t.push_back(ts);
        v.push_back(value);
        count = t.size();
        write = count;
    } else {
        const std::size_t i = write % cap;
        t[i] = ts;
        v[i] = value;
        ++write;
        count = cap;
    }
    if (!hasLatest || ts >= latestTs) {
        latest = value;
        latestTs = ts;
        hasLatest = true;
    }
}

void SignalRing::resetSeries() {
    t.clear();
    v.clear();
    write = 0;
    count = 0;
}

std::size_t SignalRing::plotOffset() const {
    if (count < cap) {
        return 0;
    }
    return write % cap;
}

std::size_t SignalRing::index(std::size_t logical) const {
    return interp::ringIndex(logical, count, cap, write);
}

double SignalRing::firstTs() const {
    return count == 0 ? 0.0 : t[index(0)];
}

double SignalRing::lastTs() const {
    return count == 0 ? 0.0 : t[index(count - 1)];
}

bool SignalRing::interpolate(double tMs, double& value) const {
    return interp::interpolate(t.data(), v.data(), count, cap, write, tMs, value);
}

void ClientArena::addDefaultRings() {
    for (const auto& desc : kCatalog) {
        SignalRing ring;
        ring.id = desc.id;
        ring.name = desc.name;
        ring.t.reserve(4096);
        ring.v.reserve(4096);
        rings_.emplace(desc.id, std::move(ring));
    }
}

ClientArena::ClientArena() {
    wsUrl = pageLiveUrl();
    addDefaultRings();
}

void ClientArena::syncRingSeams() {
    for (const auto& [id, ring] : rings_) {
        if (ring.count == 0) {
            tiles_.updateRingOldest(id, 0);
            continue;
        }
        const double ts = ring.firstTs();
        if (!std::isfinite(ts) || ts <= 0.0) {
            tiles_.updateRingOldest(id, 0);
            continue;
        }
        tiles_.updateRingOldest(id, static_cast<std::uint64_t>(ts));
    }
}

void ClientArena::clearTelemetry() {
    for (auto& [id, ring] : rings_) {
        (void)id;
        ring.t.clear();
        ring.v.clear();
        ring.write = 0;
        ring.count = 0;
        ring.hasLatest = false;
    }
    tiles_.clearRingOldest();
    timeline_ = proto::TimelineStatus{};
    alarms_.resetRuntime();
    mapsResetPulse_ = true;
}

void ClientArena::ingestTelemetry(std::span<const proto::TelemetryBlock> blocks) {
    if (catalogPinned()) {
        return;
    }
    for (const auto& block : blocks) {
        auto it = rings_.find(block.signalId);
        if (it == rings_.end()) {
            SignalRing created;
            created.id = block.signalId;
            if (const auto* desc = findSignal(block.signalId)) {
                created.name = desc->name;
            } else {
                created.name = "sig" + std::to_string(block.signalId);
            }
            it = rings_.emplace(block.signalId, std::move(created)).first;
        }
        const std::size_t n = std::min(block.timestamps.size(), block.values.size());
        if (n == 0) {
            continue;
        }
        std::size_t newest = 0;
        for (std::size_t i = 1; i < n; ++i) {
            if (block.timestamps[i] >= block.timestamps[newest]) {
                newest = i;
            }
        }
        auto& dst = it->second;
        if (!dst.hasLatest || block.timestamps[newest] >= dst.latestTs) {
            dst.latest = block.values[newest];
            dst.latestTs = block.timestamps[newest];
            dst.hasLatest = true;
        }
        // Latest-first tip then lookback: replace the 1-sample ring so time stays ordered.
        if (n > 1 && dst.count > 0 && block.timestamps[0] < dst.firstTs()) {
            dst.resetSeries();
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (dst.count > 0 && block.timestamps[i] <= dst.lastTs()) {
                continue;
            }
            dst.push(block.timestamps[i], block.values[i]);
        }
        if (!catalogPinned() && dst.count > 0) {
            const double ts = dst.firstTs();
            if (std::isfinite(ts) && ts > 0.0) {
                tiles_.updateRingOldest(block.signalId, static_cast<std::uint64_t>(ts));
            }
        }
    }
}

void ClientArena::ingestGap(std::int64_t fromNs, std::int64_t toNs) {
    if (fromNs > 0 && toNs >= fromNs) {
        tiles_.noteGap(static_cast<std::uint64_t>(fromNs / 1000000), static_cast<std::uint64_t>(toNs / 1000000));
    }
    gaps_.push_back(GapSpan{fromNs, toNs});
    constexpr std::size_t kMaxGaps = 256;
    if (gaps_.size() > kMaxGaps) {
        gaps_.erase(gaps_.begin(), gaps_.begin() + static_cast<std::ptrdiff_t>(gaps_.size() - kMaxGaps));
    }
}

void ClientArena::setLiveCursor(std::uint64_t serverNowMs) {
    if (pinned_) {
        // A pinned run owns the timeline; the live clock does not move it.
        return;
    }
    // Live: the cursor is the server's now. min is the oldest sample the rings
    // hold, so the timeline never reads as empty while data arrives.
    std::uint64_t oldest = serverNowMs;
    for (const auto& [id, ring] : rings_) {
        (void)id;
        if (ring.count == 0) {
            continue;
        }
        const double ts = ring.firstTs();
        if (std::isfinite(ts) && ts > 0.0) {
            oldest = std::min(oldest, static_cast<std::uint64_t>(ts));
        }
    }
    timeline_.mode = static_cast<std::uint8_t>(proto::TimelineMode::Live);
    timeline_.cursorMs = serverNowMs;
    timeline_.minTimeMs = oldest;
    timeline_.maxTimeMs = serverNowMs;
    setPreviewCursor(0, false);
}

CatalogChange ClientArena::applyCatalog(const ctrl2::Catalog& catalog) {
    CatalogTable table = buildCatalogTable(catalog);
    CatalogChange change;
    change.serverCount = table.serverCount;
    change.duplicates = table.duplicates;
    if (haveSignalCatalog && catalog.sha == catalogSha) {
        // The same catalog again (a reconnect): the ids still mean the same signals.
        return change;
    }
    change.changed = true;
    change.remap = remapIds(signalCatalog, table.signals);
    if (table.duplicates > 0) {
        pushLog("catalog has " + std::to_string(table.duplicates) + " repeated signal name(s), first " +
                    table.firstDuplicate + "; keeping the first of each",
                true);
    }

    // Ids changed meaning: rings, the signal map and the alarms start again.
    rings_.clear();
    addDefaultRings();
    signalCatalog = std::move(table.signals);
    signalExtra = std::move(table.extra);
    catalogBuses = std::move(table.buses);
    catalogSha = catalog.sha;
    haveSignalCatalog = true;
    for (const auto& sig : signalCatalog) {
        SignalRing& ring = rings_[sig.id];
        ring.id = sig.id;
        ring.name = sig.message.empty() ? sig.name : sig.message + "." + sig.name;
    }
    std::unordered_map<std::uint32_t, double> periods;
    for (std::size_t i = 0; i < signalCatalog.size() && i < signalExtra.size(); ++i) {
        periods[signalCatalog[i].id] = signalExtra[i].periodMs;
    }
    tiles_.setPeriods(std::move(periods));
    clearTelemetry();
    resetSignalMap();
    signalMap_.resolve(signalCatalog, *this);
    alarms_.resolve(signalCatalog, *this);
    return change;
}

void ClientArena::syncRun(HistoryIO* io) {
    // The pinned run while replaying, else the live run; rev and start come from runs_list.
    std::string runId = pinned_ ? pinnedRun_ : server.liveRun;
    std::int64_t rev = 0;
    std::uint64_t minMs = 0;
    for (const auto& run : runs) {
        if (run.id == runId) {
            rev = run.rev;
            minMs = run.startMs > 0 ? static_cast<std::uint64_t>(run.startMs) : 0;
            break;
        }
    }
    if (io != nullptr && (runId != tiles_.runId() || rev != tiles_.rev() || catalogSha != tiles_.catalogSha())) {
        tiles_.cancelAll(*io);
    }
    tiles_.setRun(runId, rev, catalogSha, minMs, timeline_.maxTimeMs);
}

float ClientArena::historyRateHz(std::uint32_t signalId) const {
    if (signalId == 0) {
        return 0.0f;
    }
    const std::uint64_t cur = effectiveCursorMs();
    if (cur == 0) {
        return 0.0f;
    }
    const std::uint64_t from = cur > 1500 ? cur - 1500 : (timeline_.minTimeMs != 0 ? timeline_.minTimeMs : 0);
    const PlotArrays arr = tiles_.gatherL0(signalId, from, cur);
    std::size_t n = 0;
    double t0 = 0.0;
    double t1 = 0.0;
    for (double t : arr.t) {
        if (!std::isfinite(t) || t <= 0.0) {
            continue;
        }
        if (n == 0) {
            t0 = t;
        }
        t1 = t;
        ++n;
    }
    if (n < 2 || t1 <= t0) {
        return 0.0f;
    }
    return static_cast<float>(static_cast<double>(n - 1) * 1000.0 / (t1 - t0));
}

void ClientArena::holdReplayCells() {
    if (!cells_.synthetic) {
        return;
    }
    cells_.synthetic = false;
    cells_.voltage.clear();
    cells_.temp.clear();
    cells_.tapTs.clear();
}

void ClientArena::updatePlaceholderCells(double nowMs) {
    const double soc = latest(kSignalSoc, 60.0);
    const double packTemp = latest(kSignalTemp, 50.0);
    const float t = static_cast<float>(nowMs * 0.001);
    cells_.synthetic = true;
    cells_.voltage.assign(kCellCount, 0.0f);
    cells_.temp.assign(kCellCount, 0.0f);
    cells_.tapTs.assign(kCellCount, 0.0);
    for (std::size_t i = 0; i < kCellCount; ++i) {
        const float phase = static_cast<float>(i);
        cells_.voltage[i] = 3.20f + static_cast<float>(soc) * 0.0105f + 0.045f * std::sin(phase * 0.70f + t);
        cells_.temp[i] = static_cast<float>(packTemp) + 2.8f * std::sin(phase * 0.45f + t * 0.35f);
    }
}

void ClientArena::pushLog(std::string text, bool error) {
    logs_.push_back(LogLine{wallMs(), error, std::move(text)});
    constexpr std::size_t kMax = 400;
    if (logs_.size() > kMax) {
        logs_.erase(logs_.begin(), logs_.begin() + static_cast<std::ptrdiff_t>(logs_.size() - kMax));
    }
}

namespace {

// Frames the decoder has not taken yet. A screen that stops drawing video stops taking
// them, so the queue is held to this many bytes, oldest frames out first.
constexpr std::size_t kVideoQueueBytes = std::size_t{8} << 20;

}  // namespace

void ClientArena::pushVideoRestart() {
    videoEvents_.push_back(VideoEvent{VideoEvent::Kind::Restart, 0, 0, {}});
    video.frames = 0;
    video.fpsCount = 0;
    video.fpsStartMs = 0.0;
    video.fps = 0.0;
}

void ClientArena::pushVideoFrame(const AssembledFrame& frame) {
    VideoEvent ev;
    ev.kind = VideoEvent::Kind::Frame;
    ev.tMs = frame.tMs;
    ev.flags = decoderFlags(frame.flags);
    ev.payload = frame.payload;
    videoBytes_ += ev.payload.size();
    videoEvents_.push_back(std::move(ev));
    while (videoBytes_ > kVideoQueueBytes) {
        const auto oldest = std::find_if(videoEvents_.begin(), videoEvents_.end(), [](const VideoEvent& e) {
            return e.kind == VideoEvent::Kind::Frame;
        });
        if (oldest == videoEvents_.end()) {
            break;
        }
        videoBytes_ -= oldest->payload.size();
        videoEvents_.erase(oldest);
    }

    ++video.frames;
    video.replay = isReplayChunk(frame.flags);
    video.gap.reset();
    if ((frame.flags & proto2::kVideoConfig) == 0) {
        if (video.fpsStartMs == 0.0) {
            video.fpsStartMs = tickMs_;
        }
        ++video.fpsCount;
        const double span = tickMs_ - video.fpsStartMs;
        if (span >= 1000.0) {
            video.fps = static_cast<double>(video.fpsCount) * 1000.0 / span;
            video.fpsCount = 0;
            video.fpsStartMs = tickMs_;
        }
    }
}

std::deque<VideoEvent> ClientArena::takeVideoEvents() {
    std::deque<VideoEvent> out;
    out.swap(videoEvents_);
    videoBytes_ = 0;
    return out;
}

void ClientArena::applyVideoStatus(const ctrl2::VideoStatus& status) {
    video.mode = status.mode;
    video.tMs = status.tMs < 0 ? 0 : static_cast<std::uint64_t>(status.tMs);
    video.clock = status.clock;
    video.qid = status.qid;
    video.haveStatus = true;
    // A plain status ends a gap; one with a gap shows it.
    video.gap = status.gap;
}

void ClientArena::resetSignalMap() {
    signalMap_.reset();
    alarms_.resetResolve();
    alarms_.resetRuntime();
}

void ClientArena::refreshMappedCells() {
    if (signalMap_.cellsMapped()) {
        signalMap_.fillCells(*this);
    }
}

void ClientArena::pinRun(const ctrl2::Run& run) {
    const auto start = run.startMs > 0 ? static_cast<std::uint64_t>(run.startMs) : std::uint64_t{0};
    auto end = run.endMs > 0 ? static_cast<std::uint64_t>(run.endMs) : std::uint64_t{0};
    if (end == 0 || end < start) {
        // A live run has no end yet: replay runs up to now.
        end = wallMs();
    }
    pinned_ = true;
    pinnedRun_ = run.id;
    timeline_.minTimeMs = start;
    timeline_.maxTimeMs = end;
    // Start at the beginning so Play walks the run. Pinning at the end would leave
    // the playhead at the last sample, paused.
    timeline_.cursorMs = start != 0 ? start : end;
    timeline_.mode = static_cast<std::uint8_t>(proto::TimelineMode::Play);
    pinnedPlayWallMs_ = 0.0;
    setPreviewCursor(0, false);
    setVideoFollowMs(timeline_.cursorMs);
    tiles_.clearRingOldest();
    sessionFollowPulse_ = true;
    // The live rings hold the DAQ, not this run: drop them so gauges and plots cannot mix the two.
    for (auto& [id, ring] : rings_) {
        (void)id;
        ring.resetSeries();
        ring.hasLatest = false;
        ring.latest = 0.0;
        ring.latestTs = 0.0;
    }
    holdReplayCells();
    pushLog("replay " + run.id);
}

void ClientArena::unpinRun() {
    if (!pinned_) {
        return;
    }
    pinned_ = false;
    pinnedRun_.clear();
    pinnedPlayWallMs_ = 0.0;
    sessionFollowPulse_ = true;
    // Back to the live clock; the next tick sets the live cursor.
    timeline_.mode = static_cast<std::uint8_t>(proto::TimelineMode::Live);
    timeline_.cursorMs = wallMs();
    timeline_.minTimeMs = 0;
    timeline_.maxTimeMs = 0;
    setPreviewCursor(0, false);
    clearVideoFollow();
    pushLog("live");
}

void ClientArena::applyPinnedTimeline(proto::TimelineMode mode, std::uint64_t cursorMs) {
    if (!pinned_) {
        return;
    }
    if (timeline_.minTimeMs != 0 && cursorMs < timeline_.minTimeMs) {
        cursorMs = timeline_.minTimeMs;
    }
    if (timeline_.maxTimeMs != 0 && cursorMs > timeline_.maxTimeMs) {
        cursorMs = timeline_.maxTimeMs;
    }
    timeline_.cursorMs = cursorMs;
    timeline_.mode = static_cast<std::uint8_t>(mode);
    setVideoFollowMs(cursorMs);
    if (mode != proto::TimelineMode::Play) {
        pinnedPlayWallMs_ = 0.0;
    }
    setPreviewCursor(0, false);
}

void ClientArena::tickPinnedPlayback(double nowMs) {
    if (!pinned_) {
        return;
    }
    if (static_cast<proto::TimelineMode>(timeline_.mode) != proto::TimelineMode::Play) {
        pinnedPlayWallMs_ = 0.0;
        return;
    }
    if (pinnedPlayWallMs_ <= 0.0) {
        pinnedPlayWallMs_ = nowMs;
        return;
    }
    const double dt = nowMs - pinnedPlayWallMs_;
    pinnedPlayWallMs_ = nowMs;
    if (!(dt > 0.0) || dt > 250.0) {
        return;
    }
    const auto step = static_cast<std::uint64_t>(dt + 0.5);
    const std::uint64_t next = timeline_.cursorMs + step;
    if (timeline_.maxTimeMs != 0 && next >= timeline_.maxTimeMs) {
        timeline_.cursorMs = timeline_.maxTimeMs;
        setVideoFollowMs(timeline_.maxTimeMs);
        timeline_.mode = static_cast<std::uint8_t>(proto::TimelineMode::Pause);
        pinnedPlayWallMs_ = 0.0;
        return;
    }
    timeline_.cursorMs = next;
    setVideoFollowMs(next);
}

void ClientArena::maybePrimeHistory(HistoryIO& io) {
    if (!haveSignalCatalog || tiles_.runId().empty()) {
        return;
    }
    auto appendMapped = [&](const char* role, std::vector<std::uint32_t>& dest) {
        const ResolvedScalar* slot = signalMap_.scalar(role);
        if (slot != nullptr && slot->mapped && slot->id != 0) {
            dest.push_back(slot->id);
        }
    };
    // OSM reads gps_* at the playhead. The old 8-id cap dropped those
    // (sorted numeric ids, BPS/pedals first) so the map stayed on COTA.
    std::vector<std::uint32_t> gpsIds;
    appendMapped("gps_lat", gpsIds);
    appendMapped("gps_lon", gpsIds);
    appendMapped("gps_valid", gpsIds);
    appendMapped("gps_speed", gpsIds);
    auto appendMux = [&](const char* role, std::vector<std::uint32_t>& dest) {
        const ResolvedMux* slot = signalMap_.mux(role);
        if (slot == nullptr || !slot->mapped) {
            return;
        }
        if (slot->indexId != 0) {
            dest.push_back(slot->indexId);
        }
        if (slot->valueId != 0) {
            dest.push_back(slot->valueId);
        }
    };
    std::vector<std::uint32_t> urgent = gpsIds;
    appendMux("cell_voltage", urgent);
    appendMux("cell_temp", urgent);
    std::vector<std::uint32_t> ids = urgent;
    signalMap_.collectMappedIds(ids);
    if (ids.empty()) {
        ids.reserve(rings_.size());
        for (const auto& [id, ring] : rings_) {
            (void)ring;
            if (id != 0) {
                ids.push_back(id);
            }
        }
    }
    if (ids.empty()) {
        return;
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    // The cursor's raw tiles: urgent for the mapped signals the readouts use, priming
    // for the rest. want() is a lookup for tiles already here, so this runs every frame.
    const std::uint64_t cursor = effectiveCursorMs();
    tiles_.requestCursorWindow(io, urgent, cursor);
    tiles_.primeStartup(io, ids, cursor);
    if (catalogPinned() && !urgent.empty()) {
        // The cell pack reads the last three minutes at the playhead.
        constexpr std::uint64_t kCellWindowMs = 180000;
        const std::uint64_t from = cursor > kCellWindowMs ? cursor - kCellWindowMs : tiles_.runMinMs();
        if (cursor > from) {
            tiles_.requestWindow(io, urgent, from, cursor, 0, false);
        }
    }
}

void ClientArena::setMappedCells(std::vector<float> voltages, std::vector<float> temps,
                                 std::vector<double> tapTs) {
    cells_.synthetic = false;
    cells_.voltage = std::move(voltages);
    cells_.temp = std::move(temps);
    cells_.tapTs = std::move(tapTs);
    if (cells_.tapTs.size() < cells_.voltage.size()) {
        cells_.tapTs.resize(cells_.voltage.size(), 0.0);
    }
}

std::vector<PlotSignal> ClientArena::plotCatalog() const {
    std::vector<PlotSignal> out;
    if (haveSignalCatalog && !signalCatalog.empty()) {
        out.reserve(signalCatalog.size() + kCatalogCount);
        for (const auto& fallback : kCatalog) {
            bool found = false;
            for (const auto& sig : signalCatalog) {
                if (sig.id == fallback.id) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                out.push_back(PlotSignal{fallback.id, fallback.label, fallback.unit});
            }
        }
        for (const auto& sig : signalCatalog) {
            PlotSignal row;
            row.id = sig.id;
            row.label = sig.message.empty() ? sig.name : sig.message + "." + sig.name;
            row.unit = sig.unit;
            out.push_back(std::move(row));
        }
        return out;
    }
    out.reserve(kCatalogCount);
    for (const auto& fallback : kCatalog) {
        out.push_back(PlotSignal{fallback.id, fallback.label, fallback.unit});
    }
    return out;
}

SignalRing* ClientArena::ring(std::uint32_t id) {
    const auto it = rings_.find(id);
    return it == rings_.end() ? nullptr : &it->second;
}

const SignalRing* ClientArena::ring(std::uint32_t id) const {
    const auto it = rings_.find(id);
    return it == rings_.end() ? nullptr : &it->second;
}

double ClientArena::latest(std::uint32_t id, double fallback) const {
    const auto* r = ring(id);
    if (r == nullptr || !r->hasLatest) {
        return fallback;
    }
    return r->latest;
}

double ClientArena::readout(std::uint32_t id, double fallback) const {
    double v = 0.0;
    if (cursorValue(id, v)) {
        return v;
    }
    return fallback;
}

bool ClientArena::cursorValue(std::uint32_t id, double& value) const {
    if (catalogPinned()) {
        const auto t = effectiveCursorMs();
        return t != 0 && tiles_.sampleNear(id, t, value);
    }
    const auto* r = ring(id);
    if (timelineIsLive()) {
        if (r != nullptr && r->hasLatest) {
            value = r->latest;
            return true;
        }
        return tiles_.sampleAt(id, effectiveCursorMs(), value);
    }
    // Before the ring's oldest sample the tiles answer; from there on the ring does.
    const auto t = effectiveCursorMs();
    const bool beforeRing = r == nullptr || r->count == 0 || static_cast<double>(t) < r->firstTs();
    if (t != 0 && beforeRing && tiles_.sampleAt(id, t, value)) {
        return true;
    }
    if (r != nullptr && r->hasLatest) {
        if (t != 0 && r->interpolate(static_cast<double>(t), value)) {
            return true;
        }
        value = r->latest;
        return true;
    }
    return t != 0 && tiles_.sampleAt(id, t, value);
}

bool ClientArena::interpolate(std::uint32_t id, double tMs, double& value) const {
    const auto* r = ring(id);
    if (r == nullptr) {
        return false;
    }
    return r->interpolate(tMs, value);
}

bool ClientArena::has(std::uint32_t id) const {
    if (catalogPinned()) {
        double v = 0.0;
        const auto t = effectiveCursorMs();
        return t != 0 && tiles_.sampleNear(id, t, v);
    }
    const auto* r = ring(id);
    if (r != nullptr && r->hasLatest) {
        return true;
    }
    double v = 0.0;
    return tiles_.sampleAt(id, effectiveCursorMs(), v);
}

bool ClientArena::consumeIndexReady() {
    const bool pulse = indexReadyPulse_;
    indexReadyPulse_ = false;
    return pulse;
}

bool ClientArena::consumeSessionFollow() {
    const bool pulse = sessionFollowPulse_;
    sessionFollowPulse_ = false;
    return pulse;
}

bool ClientArena::consumeMapsReset() {
    const bool pulse = mapsResetPulse_;
    mapsResetPulse_ = false;
    return pulse;
}

}  // namespace photon::ui
