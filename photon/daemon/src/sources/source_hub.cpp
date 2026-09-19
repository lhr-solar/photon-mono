#include "photon/source.hpp"

#include "photon/log_sessions.hpp"

#include <cstdio>

namespace photon {

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
