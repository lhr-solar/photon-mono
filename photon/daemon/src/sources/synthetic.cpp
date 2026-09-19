#include "photon/source.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace photon {
namespace {

constexpr std::size_t kRingCap = 16384;
// Live subscribe: default plot window (15 s) + mux cycle. The full 16 k ring
// used to dump minutes of history, so a second window walked old values.
constexpr std::uint64_t kLiveSnapshotLookbackMs = 15000;
constexpr std::size_t kLiveSnapshotMaxSamples = 2000;
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

}  // namespace photon
