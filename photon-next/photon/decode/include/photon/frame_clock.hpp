#pragma once

#include "photon/canp.hpp"

#include <cstdint>
#include <unordered_map>

namespace photon::frame_clock {

// The car stamps a whole CANP batch with its send time and leaves every dt at
// zero, so ~9 accel samples per 20 ms batch would share one timestamp. The IMU
// frames carry their own 16-bit counter in bytes 0-1 (big-endian) that ticks
// every 10 ms, with about four accel samples (or one gyro sample) per tick.
// FrameClock turns that counter back into one time per sample.
inline constexpr double kImuTickMs = 10.0;

// IMU_Accel_FR, IMU_Gyro_FR, IMU_B_Accel, IMU_Gyro_Back, IMU_Accel_FL, IMU_Gyro_FL.
inline bool hasImuTick(std::uint32_t canId) {
    switch (canId) {
    case 4512:
    case 4513:
    case 4528:
    case 4529:
    case 4560:
    case 4561:
        return true;
    default:
        return false;
    }
}

// One per frame stream; feed it every batch in order. A frame without a counter
// keeps the batch time. Counter frames land in (previous batch, this batch], so
// batches stay ordered and session splits and windows cut between them as
// before, and the times of one message never go backwards.
class FrameClock {
public:
    // out[i] is the time of batch.packets[i].
    void stamp(const canp::Batch& batch, std::uint64_t* out);
    // Forget everything, as after a seek or a reconnect.
    void reset();

private:
    struct Stream {
        std::uint16_t lastTick = 0;
        std::int64_t ticks = 0;    // counter unwrapped since the anchor
        double originMs = 0.0;     // time of tick 0
        int sameTick = 0;          // samples already seen at lastTick
        double perTick = 4.0;      // running samples per tick
        std::uint64_t lastOutMs = 0;
        // This batch only.
        double minMs = 0.0;
        double maxMs = 0.0;
        double shiftMs = 0.0;
        bool seen = false;
    };

    std::unordered_map<std::uint32_t, Stream> streams_;
    std::uint64_t lastBatchMs_ = 0;
    bool haveBatch_ = false;
};

}  // namespace photon::frame_clock
