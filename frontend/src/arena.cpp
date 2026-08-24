#include "photon_ui/arena.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

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
    latest = value;
    latestTs = ts;
    hasLatest = true;
}

std::size_t SignalRing::plotOffset() const {
    if (count < cap) {
        return 0;
    }
    return write % cap;
}

ClientArena::ClientArena() {
    for (const auto& desc : kCatalog) {
        SignalRing ring;
        ring.id = desc.id;
        ring.name = desc.name;
        ring.t.reserve(4096);
        ring.v.reserve(4096);
        rings_.emplace(desc.id, std::move(ring));
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
    can_.clear();
    timeline_ = proto::TimelineStatus{};
    alarms_.resetRuntime();
}

void ClientArena::ingestTelemetry(std::span<const proto::TelemetryBlock> blocks) {
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
        for (std::size_t i = 0; i < n; ++i) {
            it->second.push(block.timestamps[i], block.values[i]);
        }
    }
}

void ClientArena::ingestCan(std::span<const proto::CanMonitorItem> items) {
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

void ClientArena::ingestTimeline(const proto::TimelineStatus& status) {
    const bool wasBuffering = timeline_.mode == static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
    timeline_ = status;
    if (wasBuffering && status.mode != static_cast<std::uint8_t>(proto::TimelineMode::Buffering) &&
        status.maxTimeMs > status.minTimeMs) {
        pushLog("log_replay index ready");
    }
}

void ClientArena::ingestVideo(const proto::VideoFrame& frame) {
    video.lastTsMs = frame.timestampMs;
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
    signalCatalog = msg.catalog;
    haveSignalCatalog = true;
    for (const auto& sig : signalCatalog) {
        SignalRing& ring = rings_[sig.id];
        ring.id = sig.id;
        if (ring.name.empty()) {
            ring.name = sig.message.empty() ? sig.name : sig.message + "." + sig.name;
        }
    }
    signalMap_.resolve(signalCatalog, *this);
    alarms_.resolve(signalCatalog, *this);
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

bool ClientArena::has(std::uint32_t id) const {
    const auto* r = ring(id);
    return r != nullptr && r->hasLatest;
}

}  // namespace photon::ui
