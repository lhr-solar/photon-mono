#pragma once
#include "photon/video_index.hpp"

namespace photon::ui {
// Release in stream order. In particular, a future config/keyframe must not
// advance the watermark past delta frames that are waiting for the cursor.
template <typename Submit>
void releaseVideoThrough(const vidx::GopRing& ring, std::uint64_t releasedSeq,
                         std::uint64_t cursorMs, Submit&& submit) {
    for (const auto& nal : ring.nals()) {
        if (nal.seq <= releasedSeq) continue;
        if (nal.videoMs > cursorMs) break;
        submit(nal);
    }
}

// Live has no cursor to follow, so it gets a wall clock instead. One locate
// reply is a whole ~1 s GOP; releasing it on arrival handed WebCodecs 30
// pictures inside one UI frame and the upload path only ever shows the newest
// decoded frame, so live played at the locate rate rather than 30 fps.
//
// Behind by kLiveCatchUpMs: play faster so a backlog does not become permanent
// latency. Behind by kLiveResyncMs: the buffer got far ahead (hidden tab, a
// stall, a camera burst) and playing it out would be a replay, so re-enter on
// the newest keyframe instead.
inline constexpr std::uint64_t kLiveCatchUpMs = 700;
// Must exceed a synthetic 33 ms × ~90-NAL GOP (~3 s). 2.5 s re-entered the
// IDR every paint and Live froze on one still.
inline constexpr std::uint64_t kLiveResyncMs = 15000;
inline constexpr double kLiveMaxStepMs = 500.0;
inline constexpr double kLiveCatchUpRate = 1.5;

struct LivePacing {
    // Release through this. Only meaningful when reenter is false.
    double releaseMs = 0.0;
    // Reset the decoder onto the newest resident keyframe.
    bool reenter = false;
};

inline LivePacing paceLiveRelease(double releaseMs, std::uint64_t newestMs, double dtMs) {
    LivePacing out;
    // Re-enter only on first picture, or after a real stall (hidden tab).
    // Playing through a 1–3 s GOP is not a stall; jumping to the IDR each
    // frame is a still.
    if (releaseMs == 0 || (newestMs > releaseMs + kLiveResyncMs && dtMs > 200.0)) {
        out.reenter = true;
        return out;
    }
    if (dtMs < 0.0) {
        dtMs = 0.0;
    }
    if (dtMs > kLiveMaxStepMs) {
        dtMs = kLiveMaxStepMs;
    }
    const double rate = newestMs > releaseMs + kLiveCatchUpMs ? kLiveCatchUpRate : 1.0;
    // Preserve sub-millisecond time between paints. Truncating every 60 Hz
    // tick to 16 ms loses 40 ms each second and builds a permanent backlog.
    out.releaseMs = releaseMs + dtMs * rate;
    // Starved: hold at what is resident, so the next batch is paced from here
    // instead of being released in one tick.
    if (out.releaseMs > newestMs) {
        out.releaseMs = newestMs;
    }
    return out;
}
}
