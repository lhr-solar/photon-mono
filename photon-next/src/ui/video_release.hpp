#pragma once
#include "wire/video_index.hpp"

namespace photon::ui {
// Release in stream order. In particular, a future config/keyframe must not
// advance the watermark past delta frames that are waiting for the cursor.
template <typename Submit>
void releaseVideoThrough(const vidx::GopRing& ring, std::uint64_t releasedSeq, std::uint64_t cursorMs,
                         Submit&& submit) {
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
// A transient down status must not reset live pacing while pictures are arriving.
inline bool liveVideoRecentlyActive(bool connected, bool subscribed, bool liveMode, std::uint64_t arrivedMs,
                                    std::uint64_t wallMs) {
    return connected ||
           (subscribed && liveMode && arrivedMs != 0 && wallMs >= arrivedMs && wallMs - arrivedMs < 2000);
}

inline constexpr std::uint64_t kLivePictureStepMs = 33;
// A real camera timestamp may land a frame or two ahead. A GOP locate stamp
// jumps by the whole GOP (about 1–2 s). That jump is not a picture to wait for.
inline constexpr std::uint64_t kLiveStampSlackMs = 80;

struct LivePictureClock {
    std::uint64_t lastRawMs = 0;
    std::uint64_t nextPicMs = 0;
    bool haveVcl = false;
};

// Live locate stamps every NAL in a GOP with one wall time, then the next GOP
// jumps forward by the GOP length. Pacing will not show a picture until the
// release clock reaches its stamp, so that jump is a freeze at every keyframe.
// Keep the 33 ms picture clock across the jump. Timestamps that already step
// about one frame stay on the camera clock.
inline std::uint64_t stampLivePicture(LivePictureClock& clock, std::uint64_t timestampMs, bool newPic) {
    if (!newPic && clock.haveVcl) {
        return clock.nextPicMs;
    }
    if (!clock.haveVcl) {
        clock.nextPicMs = timestampMs;
        clock.lastRawMs = timestampMs;
        clock.haveVcl = true;
        return clock.nextPicMs;
    }
    if (timestampMs == clock.lastRawMs) {
        clock.nextPicMs += kLivePictureStepMs;
    } else if (timestampMs > clock.nextPicMs && timestampMs <= clock.nextPicMs + kLiveStampSlackMs) {
        clock.nextPicMs = timestampMs;
        clock.lastRawMs = timestampMs;
    } else {
        clock.nextPicMs += kLivePictureStepMs;
        clock.lastRawMs = timestampMs;
    }
    return clock.nextPicMs;
}

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
}  // namespace photon::ui
