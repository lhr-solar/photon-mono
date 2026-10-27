// Linux and macOS: the OSM 3D overlay (WebView2) and the H.264 decoder (Media
// Foundation) are Windows-only, so photon-ui links these instead. The 3D view
// falls back to its own scene and the video tile says decode is unavailable.
#ifndef __EMSCRIPTEN__

#include "photon_ui/platform.hpp"

#include <cstdint>
#include <cstdio>

namespace {

constexpr const char* kMapsStatus = "OSM 3D: Windows only for now";
constexpr const char* kVideoError = "video decode: Windows only for now";

}  // namespace

namespace photon::ui {

void nativeHostAttach(void* /*hwnd*/) {}

void nativeHostShutdown() {}

}  // namespace photon::ui

extern "C" {

void photon_maps3d_show(int /*on*/) {}

void photon_maps3d_layout(float /*x*/, float /*y*/, float /*w*/, float /*h*/) {}

void photon_maps3d_start(double /*lat*/, double /*lng*/, const char* /*url*/) {}

void photon_maps3d_pose(double /*lat*/, double /*lng*/, double /*heading*/, double /*scale*/, double /*range*/,
                        double /*tilt*/, int /*follow*/) {}

int photon_maps3d_ready() { return 0; }

int photon_maps3d_hosted() { return 0; }

void photon_maps3d_status(char* buf, int n) {
    if (buf == nullptr || n <= 0) {
        return;
    }
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", kMapsStatus);
}

void photon_maps3d_reset_cam() {}

void photon_maps3d_interactive(int /*on*/) {}

int photon_video_supported() { return 0; }

void photon_video_init() {}

void photon_video_reset() {}

void photon_video_copy_error(char* ptr, int cap) {
    if (ptr == nullptr || cap <= 0) {
        return;
    }
    std::snprintf(ptr, static_cast<std::size_t>(cap), "%s", kVideoError);
}

int photon_video_dim(int /*which*/) { return 0; }

void photon_video_feed(const std::uint8_t* /*ptr*/, int /*len*/, double /*tsMs*/, int /*flags*/) {}

void photon_video_end_au(int /*force*/) {}

int photon_video_upload(unsigned /*glTex*/, double /*cursorMs*/) { return 0; }

}  // extern "C"

#endif
