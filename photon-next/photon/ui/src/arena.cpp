#include "photon_ui/arena.hpp"
#include "photon_ui/page_url.hpp"

#include "photon/interp.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace photon::ui {
namespace {

bool containsFold(const std::string& hay, const char* needle) {
    if (needle == nullptr || needle[0] == '\0') {
        return false;
    }
    const std::size_t n = std::strlen(needle);
    if (n > hay.size()) {
        return false;
    }
    for (std::size_t i = 0; i + n <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < n; ++j) {
            const auto a = static_cast<unsigned char>(hay[i + j]);
            const auto b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

std::uint32_t chooseAntennaId(const std::vector<ctrl::CatalogSignal>& catalog) {
    std::uint32_t onTelemetry = 0;
    for (const ctrl::CatalogSignal& cat : catalog) {
        if (containsFold(cat.name, "antenna") || containsFold(cat.name, "rssi") || containsFold(cat.name, "snr")) {
            return cat.id;
        }
        if (onTelemetry == 0 && containsFold(cat.message, "Telemetry_Status") && !cat.name.empty()) {
            onTelemetry = cat.id;
        }
    }
    return onTelemetry;
}

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

ClientArena::ClientArena() {
    wsUrl = pageLiveUrl();
    for (const auto& desc : kCatalog) {
        SignalRing ring;
        ring.id = desc.id;
        ring.name = desc.name;
        ring.t.reserve(4096);
        ring.v.reserve(4096);
        rings_.emplace(desc.id, std::move(ring));
    }
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
    can_.clear();
    timeline_ = proto::TimelineStatus{};
    alarms_.resetRuntime();
    primedHistoryKey_.clear();
    mapsResetPulse_ = true;
}

void ClientArena::ingestTelemetry(std::span<const proto::TelemetryBlock> blocks) {
    if (catalogPinned_) {
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
        if (!catalogPinned_ && dst.count > 0) {
            const double ts = dst.firstTs();
            if (std::isfinite(ts) && ts > 0.0) {
                tiles_.updateRingOldest(block.signalId, static_cast<std::uint64_t>(ts));
            }
        }
    }
}

void ClientArena::ingestCan(std::span<const proto::CanMonitorItem> items) {
    if (catalogPinned_) {
        return;
    }
    for (const auto& item : items) {
        CanRow& row = can_[item.canId];
        row.canId = item.canId;
        row.dlc = item.dlc;
        row.flags = item.flags;
        std::copy(item.data, item.data + 8, row.data);
        row.lastTsMs = item.timestampMs;
        ++row.count;
        if (row.windowStartMs == 0 || item.timestampMs < row.windowStartMs) {
            row.windowStartMs = item.timestampMs;
            row.windowCount = 0;
        }
        ++row.windowCount;
        const std::uint64_t dt = item.timestampMs - row.windowStartMs;
        if (dt >= 1000) {
            row.rateHz = static_cast<float>(row.windowCount) * 1000.0f / static_cast<float>(dt);
            row.windowCount = 0;
            row.windowStartMs = item.timestampMs;
        }
    }
}

void ClientArena::pinCatalogSession(const ctrl::Message::CatalogSession& row) {
    catalogPinned_ = true;
    pinnedCatalogId_ = row.id;
    timeline_.minTimeMs = row.minMs;
    timeline_.maxTimeMs = row.maxMs;
    // Start at the beginning so Play can walk the run. Pinning at maxMs left
    // the playhead at EOF (PAUSE forever, CAN Hz frozen).
    timeline_.cursorMs = row.minMs != 0 ? row.minMs : row.maxMs;
    timeline_.mode = static_cast<std::uint8_t>(proto::TimelineMode::Play);
    pinnedPlayWallMs_ = 0.0;
    setPreviewCursor(0, false);
    setVideoFollowMs(timeline_.cursorMs);
    tiles_.setSession(row.id, row.dbcHash, row.minMs, row.maxMs);
    tiles_.setCompactFetch(true);
    tiles_.clearRingOldest();
    primedHistoryKey_.clear();
    sessionFollowPulse_ = true;
    // Drop live/synth rings so gauges and 3D cannot mix DAQ with the log.
    for (auto& [id, live] : rings_) {
        (void)id;
        live.resetSeries();
        live.hasLatest = false;
        live.latest = 0.0;
        live.latestTs = 0.0;
    }
    can_.clear();
    holdReplayCells();
    pushLog(std::string("catalog pin ") + row.id);
}

void ClientArena::clearCatalogPin() {
    const bool wasPinned = catalogPinned_;
    catalogPinned_ = false;
    pinnedCatalogId_.clear();
    pinnedPlayWallMs_ = 0.0;
    tiles_.setCompactFetch(false);
    sessionFollowPulse_ = true;
    if (wasPinned) {
        // Drop the run playhead immediately. Leaving min/max/cursor on the pin
        // made Live chrome still read 10:36 until (or unless) a hub tick arrived.
        timeline_.mode = static_cast<std::uint8_t>(proto::TimelineMode::Live);
        timeline_.cursorMs = wallMs();
        timeline_.minTimeMs = 0;
        timeline_.maxTimeMs = 0;
        setPreviewCursor(0, false);
        clearVideoFollow();
    }
}

void ClientArena::applyPinnedTimeline(proto::TimelineMode mode, std::uint64_t cursorMs) {
    if (!catalogPinned_) {
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
    if (!catalogPinned_) {
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

void ClientArena::ingestTimeline(const proto::TimelineStatus& status) {
    if (catalogPinned_) {
        return;
    }
    const bool wasBuffering = timeline_.mode == static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
    timeline_ = status;
    if (status.mode == static_cast<std::uint8_t>(proto::TimelineMode::Live)) {
        setPreviewCursor(0, false);
    }
    if (wasBuffering && status.mode != static_cast<std::uint8_t>(proto::TimelineMode::Buffering) &&
        status.maxTimeMs > status.minTimeMs) {
        indexReadyPulse_ = true;
        pushLog("log_replay index ready");
    }
    syncRingSeams();
    if (status.maxTimeMs > status.minTimeMs) {
        std::string id = tiles_.sessionId();
        if (id.empty() || id == "default") {
            for (const auto& day : driveDays) {
                for (const auto& sess : day.sessions) {
                    if (sess.id.empty() || sess.startMs == 0) {
                        continue;
                    }
                    const auto end = sess.startMs + static_cast<std::uint64_t>(sess.durationS * 1000.0);
                    if (status.minTimeMs >= sess.startMs && status.minTimeMs <= end) {
                        id = sess.id;
                    }
                }
            }
        }
        tiles_.setSession(id, dbcId.empty() ? tiles_.dbcHash() : dbcId, status.minTimeMs, status.maxTimeMs);
        syncRingSeams();
    }
}

void ClientArena::ingestVideo(const proto::VideoFrame& frame) {
    video.lastTsMs = frame.timestampMs;
    // SPS/PPS are restamped on every access unit. Counting them made a 30 fps
    // camera read as ~90 fps. Config NALs are not pictures.
    if (proto::videoConfig(frame.flags)) {
        return;
    }
    if (video.fpsWindowCount != 0 && frame.timestampMs == video.fpsLastPictureMs) {
        return;
    }
    video.fpsLastPictureMs = frame.timestampMs;
    ++video.frames;
    ++video.fpsWindowCount;
    const std::uint64_t now = wallMs();
    if (video.fpsWindowStartMs == 0) {
        video.fpsWindowStartMs = now;
    }
    const std::uint64_t dt = now - video.fpsWindowStartMs;
    if (dt >= 1000) {
        video.fps = static_cast<double>(video.fpsWindowCount) * 1000.0 / static_cast<double>(dt);
        video.fpsWindowCount = 0;
        video.fpsWindowStartMs = now;
    }
}

void ClientArena::applyVideoStatus(const ctrl::Message& msg) {
    video.connected = msg.videoConnected;
    video.host = msg.host;
    video.port = msg.port;
    video.mode = std::string(ctrl::videoModeName(msg.videoMode));
    video.lastTsMs = msg.lastTsMs;
    video.frames = msg.videoFrames;
    video.dropped = msg.videoDropped;
    video.haveStatus = true;
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

void ClientArena::applyTrackSimList(const ctrl::Message& msg) {
    trackSim.tracks = msg.tracks;
    trackSim.measured = msg.measured;
    trackSim.catalogReady = true;
    trackSim.catalogPending = false;
}

void ClientArena::applyTrackSimResult(const ctrl::Message& msg) {
    trackSim.solving = false;
    trackSim.haveResult = true;
    trackSim.error.clear();
    trackSim.trackId = msg.trackId;
    trackSim.feasible = msg.feasible;
    trackSim.totalTimeS = msg.totalTimeS;
    trackSim.energyWh = msg.energyWh;
    trackSim.endSoc = msg.endSoc;
    trackSim.dEdT = msg.dEdT;
    trackSim.segments = msg.trackSegments;
}

void ClientArena::applyControlError(const ctrl::Message& msg) {
    if (!trackSim.solving && !trackSim.catalogPending) {
        return;
    }
    trackSim.solving = false;
    trackSim.catalogPending = false;
    if (!msg.code.empty() && !msg.message.empty()) {
        trackSim.error = msg.code + ": " + msg.message;
    } else if (!msg.message.empty()) {
        trackSim.error = msg.message;
    } else if (!msg.code.empty()) {
        trackSim.error = msg.code;
    } else {
        trackSim.error = "error_alert";
    }
}

void ClientArena::resetTrackSimCatalog() {
    trackSim.catalogReady = false;
    trackSim.catalogPending = false;
}

void ClientArena::beginTrackSimSolve(double timeBudgetS) {
    trackSim.solving = true;
    trackSim.error.clear();
    trackSim.timeBudgetS = timeBudgetS;
}

void ClientArena::applySignalCatalog(const ctrl::Message& msg) {
    const bool switched = haveSignalCatalog && !msg.dbcId.empty() && !dbcId.empty() && msg.dbcId != dbcId;
    if (switched) {
        rings_.clear();
        can_.clear();
        resetSignalMap();
        alarms_.resetRuntime();
    }
    if (!msg.dbcId.empty()) {
        dbcId = msg.dbcId;
    }
    if (!msg.dbcPath.empty()) {
        dbcPath = msg.dbcPath;
    }
    signalCatalog = msg.catalog;
    haveSignalCatalog = true;
    antennaId_ = chooseAntennaId(signalCatalog);
    for (const auto& sig : signalCatalog) {
        SignalRing& ring = rings_[sig.id];
        ring.id = sig.id;
        if (ring.name.empty()) {
            ring.name = sig.message.empty() ? sig.name : sig.message + "." + sig.name;
        }
    }
    signalMap_.resolve(signalCatalog, *this);
    alarms_.resolve(signalCatalog, *this);
    if (tiles_.sessionMaxMs() > tiles_.sessionMinMs()) {
        tiles_.setSession(tiles_.sessionId(), dbcId.empty() ? tiles_.dbcHash() : dbcId, tiles_.sessionMinMs(),
                          tiles_.sessionMaxMs());
    }
}

const ctrl::CatalogSignal* ClientArena::antennaSignal() const {
    if (antennaId_ == 0) {
        return nullptr;
    }
    for (const ctrl::CatalogSignal& cat : signalCatalog) {
        if (cat.id == antennaId_) {
            return &cat;
        }
    }
    return nullptr;
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

void ClientArena::maybePrimeHistory(HistoryIO& io) {
    if (!haveSignalCatalog || tiles_.sessionMaxMs() <= tiles_.sessionMinMs()) {
        return;
    }
    // Live/BUFFERING: do not dump TSDB tiles into WASM. A log switch used to
    // prime on the first BUFFERING timeline (min/max still growing) and abort.
    // Catalog pin is Pause + a known session window: prime that cursor only.
    const auto mode = static_cast<proto::TimelineMode>(timeline_.mode);
    if (!catalogPinned_ && (mode == proto::TimelineMode::Live || mode == proto::TimelineMode::Buffering ||
                            mode == proto::TimelineMode::Unknown)) {
        return;
    }
    const std::string key = tiles_.sessionId() + "|" + tiles_.dbcHash() + "|" +
                            std::to_string(tiles_.sessionMinMs()) + "|" + std::to_string(tiles_.sessionMaxMs());
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
    // Stay on the playhead. A one-shot prime at session minMs misses the first
    // samples, and a failed hop (history-remote down) used to never retry.
    if (catalogPinned_) {
        if (!urgent.empty()) {
            tiles_.requestCursorWindow(io, urgent, effectiveCursorMs());
        }
        tiles_.requestCursorWindow(io, ids, effectiveCursorMs());
    }
    if (key == primedHistoryKey_) {
        return;
    }
    if (catalogPinned_ && !gpsIds.empty() && tiles_.sessionMaxMs() > tiles_.sessionMinMs()) {
        tiles_.requestWindow(io, gpsIds, tiles_.sessionMinMs(), tiles_.sessionMaxMs(), 4000, false);
        std::fprintf(stderr, "[maps] prime GPS ids=%zu session=%s %llu..%llu\n", gpsIds.size(),
                     tiles_.sessionId().c_str(),
                     static_cast<unsigned long long>(tiles_.sessionMinMs()),
                     static_cast<unsigned long long>(tiles_.sessionMaxMs()));
        for (const auto id : gpsIds) {
            std::fprintf(stderr, "[maps] prime GPS id=%u\n", id);
        }
        std::fflush(stderr);
    }
    if (catalogPinned_) {
        // First minutes are often empty at minMs (video/CAN start skew). Pull
        // L0 tiles, not a 4k-point rollup. Mux tap index averaged over 4 s
        // cannot rebuild the 32-cell pack.
        const auto from = tiles_.sessionMinMs();
        const auto cap = from + 180000;
        const auto to = tiles_.sessionMaxMs() < cap ? tiles_.sessionMaxMs() : cap;
        if (to > from && !urgent.empty()) {
            constexpr std::uint64_t kL0Span = 60000;
            for (auto t = from; t <= to; t += kL0Span) {
                tiles_.requestCursorWindow(io, urgent, t);
                if (t > to - kL0Span) {
                    break;
                }
            }
            tiles_.requestCursorWindow(io, urgent, to);
        }
    } else {
        tiles_.primeStartup(io, ids, effectiveCursorMs());
    }
    primedHistoryKey_ = key;
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
    if (catalogPinned_) {
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
    const auto t = effectiveCursorMs();
    const auto seam = tiles_.seamFor(id);
    if (t != 0 && (seam == 0 || t < seam) && tiles_.sampleAt(id, t, value)) {
        return true;
    }
    if (r != nullptr && r->hasLatest) {
        if (t != 0 && r->interpolate(static_cast<double>(t), value)) {
            return true;
        }
        value = r->latest;
        return true;
    }
    if (t != 0 && tiles_.sampleAt(id, t, value)) {
        return true;
    }
    return false;
}

bool ClientArena::interpolate(std::uint32_t id, double tMs, double& value) const {
    const auto* r = ring(id);
    if (r == nullptr) {
        return false;
    }
    return r->interpolate(tMs, value);
}

bool ClientArena::has(std::uint32_t id) const {
    if (catalogPinned_) {
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
