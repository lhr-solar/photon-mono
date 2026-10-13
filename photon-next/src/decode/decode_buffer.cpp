#include "decode/decode_buffer.hpp"

#include <algorithm>

namespace photon {
namespace {

constexpr std::size_t kRingCap = 16384;
// Live subscribe: default plot window (15 s) + mux cycle. The full 16 k ring
// used to dump minutes of history, so a second window walked old values.
constexpr std::uint64_t kLiveSnapshotLookbackMs = 15000;
constexpr std::size_t kLiveSnapshotMaxSamples = 2000;

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

}  // namespace photon
