#pragma once

#include "imgui.h"
#include "photon/protocol.hpp"
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
//      Queue > 3 drops to the latest frame only.
//   4. Each UI frame, the latest VideoFrame is uploaded with
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

    bool hasFrame() const { return haveFrame_; }
    unsigned texture() const { return tex_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    unsigned tex_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool haveFrame_ = false;
    bool inited_ = false;
    char error_[160]{};

    void ensureTexture();
};

VideoDecoder& videoDecoder();

// Compact letterboxed video + one-line status. Pit dock tile calls this.
void drawVideoTile(const ClientArena& arena, ImVec2 size);

// Shared letterbox blit used by the Live View screen and the tile.
void drawVideoImage(ImVec2 avail);

}  // namespace photon::ui
