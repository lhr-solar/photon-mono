#include "photon/frame_clock.hpp"

#include <algorithm>
#include <cmath>

namespace photon::frame_clock {

namespace {

// Batches arrive about every 20 ms. A longer hole, or time going backwards, is a
// seek, a reconnect, or a new log: start over rather than bridge it.
constexpr std::uint64_t kResetGapMs = 5000;
// More than a second of ticks between two samples of one message means the
// sensor restarted or frames were lost; re-anchor that message.
constexpr int kMaxTickStep = 100;

}  // namespace

void FrameClock::reset() {
    streams_.clear();
    lastBatchMs_ = 0;
    haveBatch_ = false;
}

void FrameClock::stamp(const canp::Batch& batch, std::uint64_t* out) {
    const std::uint64_t hi = batch.timestamp;
    if (haveBatch_ && (hi < lastBatchMs_ || hi - lastBatchMs_ > kResetGapMs)) {
        reset();
    }
    // Every frame of this batch was read after the previous batch went out. The
    // first batch after a reset has no such bound, so it keeps the batch time.
    const std::uint64_t lo = haveBatch_ && lastBatchMs_ < hi ? lastBatchMs_ + 1 : hi;
    haveBatch_ = true;
    lastBatchMs_ = hi;

    // Pass 1: each counter frame's time on its message's own timeline.
    double raw[canp::kMaxBatch] = {};
    const std::uint16_t count = std::min<std::uint16_t>(batch.count, canp::kMaxBatch);
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto& p = batch.packets[i];
        out[i] = hi;
        if (p.dlc < 2 || !hasImuTick(p.canId)) {
            continue;
        }
        const auto tick = static_cast<std::uint16_t>((p.data[0] << 8) | p.data[1]);
        auto [it, fresh] = streams_.try_emplace(p.canId);
        Stream& s = it->second;
        const int step = static_cast<std::int16_t>(static_cast<std::uint16_t>(tick - s.lastTick));
        if (fresh || step < 0 || step > kMaxTickStep) {
            s.ticks = 0;
            s.sameTick = 0;
            s.originMs = static_cast<double>(lo);
        } else if (step == 0) {
            ++s.sameTick;
        } else {
            if (step == 1) {
                s.perTick = 0.9 * s.perTick + 0.1 * (s.sameTick + 1);
            }
            s.sameTick = 0;
            s.ticks += step;
        }
        s.lastTick = tick;
        // Samples that share a tick are spread across it at the running rate.
        const double spacing = kImuTickMs / std::max(1.0, s.perTick);
        const double within = std::min(s.sameTick * spacing, kImuTickMs - 1.0);
        raw[i] = s.originMs + static_cast<double>(s.ticks) * kImuTickMs + within;
        if (!s.seen) {
            s.minMs = raw[i];
            s.maxMs = raw[i];
            s.seen = true;
        } else {
            s.minMs = std::min(s.minMs, raw[i]);
            s.maxMs = std::max(s.maxMs, raw[i]);
        }
    }

    // Pass 2: slide each message's timeline as a whole, just enough to land in
    // this batch's window. Moving the group keeps the spacing between its
    // samples, and the slide absorbs drift between the IMU and car clocks.
    for (auto& [id, s] : streams_) {
        if (!s.seen || lo >= hi) {
            continue;
        }
        double shift = 0.0;
        if (s.maxMs > static_cast<double>(hi)) {
            shift = static_cast<double>(hi) - s.maxMs;
        }
        if (s.minMs + shift < static_cast<double>(lo)) {
            // Up to the window start, but never past its end.
            shift += std::min(static_cast<double>(lo) - (s.minMs + shift),
                              static_cast<double>(hi) - (s.maxMs + shift));
        }
        s.originMs += shift;
        s.shiftMs = shift;
    }

    for (std::uint16_t i = 0; i < count; ++i) {
        const auto& p = batch.packets[i];
        if (p.dlc < 2 || !hasImuTick(p.canId)) {
            continue;
        }
        Stream& s = streams_[p.canId];
        std::uint64_t t = hi;
        if (lo < hi) {
            t = static_cast<std::uint64_t>(std::llround(raw[i] + s.shiftMs));
            t = std::clamp(t, lo, hi);
        }
        t = std::max(t, s.lastOutMs);
        s.lastOutMs = t;
        out[i] = t;
    }
    for (auto& [id, s] : streams_) {
        s.seen = false;
        s.shiftMs = 0.0;
    }
}

}  // namespace photon::frame_clock
