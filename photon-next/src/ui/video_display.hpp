#pragma once

namespace photon::ui {
enum class VideoDisplayState { Idle, Buffering, Ready, Stalled };

// Readiness comes from an uploaded picture, never a transport FPS/status reply.
// Keep the last texture through a live interruption, but do not call it playing.
inline VideoDisplayState videoDisplayState(bool expected, bool hasFrame, bool live,
                                           double lastPresentedSeconds, double nowSeconds) {
    if (hasFrame) {
        if (live && nowSeconds - lastPresentedSeconds >= 2.0) return VideoDisplayState::Stalled;
        return VideoDisplayState::Ready;
    }
    return expected ? VideoDisplayState::Buffering : VideoDisplayState::Idle;
}
}  // namespace photon::ui
