#pragma once

#include "imgui.h"
#include "photon/protocol.hpp"
#include "photon/video_index.hpp"
#include "photon_ui/arena.hpp"

#include <cstdint>
#include <span>

namespace photon::ui {

// In-browser H.264 decode via WebCodecs VideoDecoder (EM_JS) and upload into a
// GLES texture for ImGui::Image.
//
// Render path (documented):
//   1. Feature-detect `VideoDecoder`. If missing, screens show
//      "WebCodecs unsupported in this browser".
//   2. Config-flag NALs (SPS/PPS) derive `avc1.PPCCLL` and configure the
//      decoder with annexb (no avcC description).
//   3. Key/delta NALs become EncodedVideoChunk; output VideoFrames are queued.
//      Pending frames stay in decode order (cap 12). Each UI paint uploads
//      the oldest pending frame so a GOP does not collapse to one still.
//      Live still releases NALs on a wall clock; replay follows the cursor.
//   4. Each UI frame, one VideoFrame is uploaded with
//      `texImage2D(..., videoFrame)` on the Emscripten GL texture
//      (`GL.textures[id]`). If that throws, fall back to OffscreenCanvas 2D
//      `drawImage` then `texImage2D` from the canvas.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool supported() const;
    const char* error() const { return error_; }

    void feed(std::uint64_t timestampMs, std::uint8_t flags, std::span<const std::uint8_t> nal);
    void poll();
    void reset();
    // True when the ring already has a keyframe and pictures through videoMs,
    // so Play/Pause must not re-locate (that dumps a GOP and desyncs CAN).
    bool hasVideoAt(std::uint64_t videoMs) const;

    bool hasFrame() const { return haveFrame_; }
    unsigned texture() const { return tex_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // --- cursor-driven playback (docs/pdr/tsdb-architecture.md §8.3) ---
    //
    // Video is a follower of the telemetry cursor, not a second timeline with
    // its own clock. Arriving NALs go into a bounded ring and are handed to
    // WebCodecs only when the cursor has reached them, which is what makes a
    // scrub replay video instead of fighting it.
    //
    // Call once per UI frame, after the network poll.
    void syncToCursor(const ClientArena& arena);

    // Feed a video_locate reply (payload without its u32 length prefix).
    bool applyLocateReply(std::span<const std::uint8_t> payload);

    // True when playback needs a pointer it does not have. Client::tick sends
    // video_locate; the backend replies with MsgType 0x07 and applyLocateReply
    // consumes it. The status line shows "locating…" while this is true.
    bool pendingLocate(std::uint64_t& atMs) const;
    void requestLocate(std::uint64_t atMs);
    void cancelPendingLocate();
    void resumeLive();
    void setClockOffsetMs(std::int64_t offsetMs);
    void setReplayWindow(std::uint64_t minCursorMs, std::uint64_t maxCursorMs);
    void clearReplayWindow();

    const vidx::CursorPlayback& playback() const { return playback_; }
    vidx::PlaybackMode mode() const { return mode_; }
    std::uint64_t releasedMs() const { return releasedMs_; }
    std::size_t bufferedBytes() const { return ring_.bytes(); }
    std::uint64_t bufferedNewestMs() const { return ring_.newestMs(); }

private:
    unsigned tex_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool haveFrame_ = false;
    bool inited_ = false;
    char error_[160]{};

    vidx::CursorPlayback playback_;
    vidx::GopRing ring_;
    vidx::PlaybackMode mode_ = vidx::PlaybackMode::Live;
    std::uint64_t releasedSeq_ = 0;
    std::uint64_t releasedMs_ = 0;
    std::uint64_t pendingLocateMs_ = 0;
    bool havePendingLocate_ = false;
    std::uint64_t lastRawMs_ = 0;
    std::uint64_t nextPicMs_ = 0;
    bool haveVcl_ = false;
    std::uint64_t lastCursorVideoMs_ = 0;
    std::uint64_t batchFloorMs_ = 0;
    bool pastVideoEnd_ = false;
    std::uint64_t videoEndMs_ = 0;
    std::uint64_t liveIgnoreOlderThanMs_ = 0;
    std::uint64_t lastLiveKeyMs_ = 0;
    bool skipLiveGop_ = false;
    int forcedStillTries_ = 0;
    // Live pacing. A locate reply carries a whole ~1 s GOP, so submitting on
    // arrival handed WebCodecs 30 pictures at once and the upload path only
    // ever showed the newest one: live ran at the locate rate, not 30 fps.
    double liveReleaseMs_ = 0.0;
    double liveWallMs_ = 0.0;

    void ensureTexture();
    void pushToRing(std::uint64_t timestampMs, std::uint8_t flags, std::span<const std::uint8_t> nal);
    void submit(const vidx::BufferedNal& nal);
    void releaseThrough(std::uint64_t videoMs);
    void reenterAt(std::uint64_t keyframeVideoMs, std::uint64_t feedUpToMs);
    // Hand live pictures to the decoder on a wall clock instead of on arrival.
    void releaseLivePaced(double nowMs);
};

VideoDecoder& videoDecoder();

// Compact letterboxed video + one-line status. Pit dock tile calls this.
void drawVideoTile(ClientArena& arena, ImVec2 size);

// Shared letterbox blit used by the Live View screen and the tile.
void drawVideoImage(ImVec2 avail);

// Letterboxed decoder texture, 90° CCW then vertical flip.
void blitDecodedVideo(ImVec2 avail);

// Spinning buffer indicator while a GOP or decoded picture has not arrived.
void drawVideoBuffering(ImVec2 avail);

// Same blit, but drives cursor-driven playback first. Any screen showing video
// on a scrubbable timeline should call this one, or its video keeps running on
// the decoder's arrival order instead of following the cursor.
void drawVideoImage(ClientArena& arena, ImVec2 avail);

// Analog speedo + F/N/R and status icons in a column beside the video.
void drawVideoHud(ClientArena& arena, ImVec2 colMin, ImVec2 colMax);

}  // namespace photon::ui
