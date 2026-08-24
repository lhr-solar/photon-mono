#include "photon_ui/video.hpp"

#include "photon_ui/theme.hpp"

#include "imgui.h"

#include <emscripten.h>

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace photon::ui {
namespace {

// WebCodecs + GL glue. See video.hpp for the decode/render path.
EM_JS(int, photon_video_supported, (), { return typeof VideoDecoder === 'function' ? 1 : 0; });

EM_JS(void, photon_video_init, (), {
    if (!Module._photonVideo) {
        Module._photonVideo = {
            decoder: null,
            pending: [],
            sps: null,
            pps: null,
            configured: false,
            error: "",
            width: 0,
            height: 0,
            canvas: null,
            ctx2d: null,
        };
    }
});

EM_JS(void, photon_video_reset, (), {
    const s = Module._photonVideo;
    if (!s) {
        return;
    }
    if (s.decoder) {
        try {
            s.decoder.close();
        } catch (e) {
        }
    }
    for (const f of s.pending) {
        try {
            f.close();
        } catch (e) {
        }
    }
    s.decoder = null;
    s.pending = [];
    s.sps = null;
    s.pps = null;
    s.configured = false;
    s.error = "";
    s.width = 0;
    s.height = 0;
});

EM_JS(void, photon_video_copy_error, (int ptr, int cap), {
    const s = Module._photonVideo;
    const msg = s && s.error ? s.error : "";
    stringToUTF8(msg, ptr, cap);
});

EM_JS(int, photon_video_dim, (int which), {
    const s = Module._photonVideo;
    if (!s) {
        return 0;
    }
    return which === 0 ? (s.width | 0) : (s.height | 0);
});

EM_JS(void, photon_video_feed, (int ptr, int len, double tsMs, int flags), {
    const s = Module._photonVideo;
    if (!s) {
        return;
    }
    if (typeof VideoDecoder !== 'function') {
        s.error = 'WebCodecs unsupported in this browser';
        return;
    }

    const nal = HEAPU8.slice(ptr, ptr + len);

    const startLen = function(bytes) {
        if (bytes.length >= 4 && bytes[0] === 0 && bytes[1] === 0 && bytes[2] === 0 && bytes[3] === 1) {
            return 4;
        }
        if (bytes.length >= 3 && bytes[0] === 0 && bytes[1] === 0 && bytes[2] === 1) {
            return 3;
        }
        return 0;
    };
    const nalType = function(bytes) {
        const i = startLen(bytes);
        return i < bytes.length ? (bytes[i] & 0x1f) : 0;
    };
    const codecFromSps = function(bytes) {
        const i = startLen(bytes) + 1;
        if (i + 2 >= bytes.length) {
            return 'avc1.42E01E';
        }
        const hex = function(n) { return ('0' + (n & 0xff).toString(16)).slice(-2).toUpperCase(); };
        return 'avc1.' + hex(bytes[i]) + hex(bytes[i + 1]) + hex(bytes[i + 2]);
    };
    const ensureDecoder = function() {
        if (s.decoder) {
            return true;
        }
        try {
            s.decoder = new VideoDecoder({
                output: function(frame) {
                    s.pending.push(frame);
                    if (s.pending.length > 3) {
                        const latest = s.pending.pop();
                        for (const f of s.pending) {
                            try {
                                f.close();
                            } catch (e) {
                            }
                        }
                        s.pending = [latest];
                    }
                },
                error: function(err) {
                    s.error = err && err.message ? String(err.message) : String(err);
                },
            });
        } catch (e) {
            s.error = e && e.message ? String(e.message) : String(e);
            return false;
        }
        return true;
    };

    if ((flags & 2) !== 0) {
        const t = nalType(nal);
        if (t === 7) {
            s.sps = nal;
            s.configured = false;
        } else if (t === 8) {
            s.pps = nal;
        }
        return;
    }

    if (!ensureDecoder()) {
        return;
    }

    if ((flags & 1) !== 0 && !s.configured && s.sps) {
        try {
            s.decoder.configure({codec: codecFromSps(s.sps), optimizeForLatency: true});
            s.configured = true;
            s.error = "";
        } catch (e) {
            s.error = e && e.message ? String(e.message) : String(e);
            return;
        }
    }
    if (!s.configured || !s.decoder) {
        return;
    }

    let payload = nal;
    if ((flags & 1) !== 0 && s.sps && s.pps) {
        payload = new Uint8Array(s.sps.length + s.pps.length + nal.length);
        payload.set(s.sps, 0);
        payload.set(s.pps, s.sps.length);
        payload.set(nal, s.sps.length + s.pps.length);
    }

    try {
        s.decoder.decode(new EncodedVideoChunk({
            type: (flags & 1) !== 0 ? 'key' : 'delta',
            timestamp: Math.round(tsMs * 1000),
            data: payload,
        }));
    } catch (e) {
        s.error = e && e.message ? String(e.message) : String(e);
    }
});

EM_JS(int, photon_video_upload, (int glTex), {
    const s = Module._photonVideo;
    if (!s || s.pending.length === 0) {
        return 0;
    }
    const frame = s.pending.pop();
    for (const f of s.pending) {
        try {
            f.close();
        } catch (e) {
        }
    }
    s.pending = [];
    s.width = frame.displayWidth | 0;
    s.height = frame.displayHeight | 0;
    const tex = (typeof GL !== 'undefined' && GL.textures) ? GL.textures[glTex] : null;
    if (!tex || typeof GLctx === 'undefined') {
        try {
            frame.close();
        } catch (e) {
        }
        return 0;
    }
    GLctx.bindTexture(GLctx.TEXTURE_2D, tex);
    try {
        // Preferred path: WebGL2 texImage2D accepts VideoFrame directly.
        GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA, GLctx.UNSIGNED_BYTE, frame);
    } catch (e) {
        // Fallback: 2D blit then upload the canvas.
        if (!s.canvas) {
            s.canvas = new OffscreenCanvas(frame.displayWidth, frame.displayHeight);
            s.ctx2d = s.canvas.getContext('2d', {alpha: false});
        }
        if (s.canvas.width !== frame.displayWidth) {
            s.canvas.width = frame.displayWidth;
        }
        if (s.canvas.height !== frame.displayHeight) {
            s.canvas.height = frame.displayHeight;
        }
        s.ctx2d.drawImage(frame, 0, 0);
        GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA, GLctx.UNSIGNED_BYTE, s.canvas);
    }
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_MIN_FILTER, GLctx.LINEAR);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_MAG_FILTER, GLctx.LINEAR);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_WRAP_S, GLctx.CLAMP_TO_EDGE);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_WRAP_T, GLctx.CLAMP_TO_EDGE);
    try {
        frame.close();
    } catch (e) {
    }
    return 1;
});

VideoDecoder gDecoder;

}  // namespace

VideoDecoder& videoDecoder() { return gDecoder; }

VideoDecoder::~VideoDecoder() {
    reset();
    if (tex_ != 0) {
        GLuint tex = tex_;
        glDeleteTextures(1, &tex);
        tex_ = 0;
    }
}

bool VideoDecoder::supported() const { return photon_video_supported() != 0; }

void VideoDecoder::ensureTexture() {
    if (tex_ != 0) {
        return;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    tex_ = tex;
    glBindTexture(GL_TEXTURE_2D, tex);
    const unsigned char px[4] = {16, 16, 20, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void VideoDecoder::feed(std::uint64_t timestampMs, std::uint8_t flags, std::span<const std::uint8_t> nal) {
    if (!inited_) {
        photon_video_init();
        inited_ = true;
    }
    ensureTexture();
    if (nal.empty()) {
        return;
    }
    photon_video_feed(reinterpret_cast<int>(nal.data()), static_cast<int>(nal.size()),
                      static_cast<double>(timestampMs), static_cast<int>(flags));
    photon_video_copy_error(reinterpret_cast<int>(error_), static_cast<int>(sizeof(error_)));
}

void VideoDecoder::poll() {
    if (!inited_) {
        photon_video_init();
        inited_ = true;
    }
    ensureTexture();
    if (photon_video_upload(static_cast<int>(tex_)) != 0) {
        haveFrame_ = true;
        width_ = photon_video_dim(0);
        height_ = photon_video_dim(1);
    }
    photon_video_copy_error(reinterpret_cast<int>(error_), static_cast<int>(sizeof(error_)));
}

void VideoDecoder::reset() {
    if (inited_) {
        photon_video_reset();
    }
    haveFrame_ = false;
    width_ = 0;
    height_ = 0;
    error_[0] = '\0';
}

void drawVideoImage(ImVec2 avail) {
    videoDecoder().poll();
    if (!videoDecoder().supported()) {
        ImGui::TextColored(theme::warning(), "WebCodecs unsupported in this browser");
        return;
    }
    if (videoDecoder().error()[0] != '\0' && !videoDecoder().hasFrame()) {
        ImGui::TextColored(theme::danger(), "%s", videoDecoder().error());
        return;
    }
    if (!videoDecoder().hasFrame() || videoDecoder().width() <= 0 || videoDecoder().height() <= 0) {
        ImGui::TextColored(theme::mutedFg(), "Waiting for video...");
        return;
    }
    const float vw = static_cast<float>(videoDecoder().width());
    const float vh = static_cast<float>(videoDecoder().height());
    const float scale = std::min(avail.x / vw, avail.y / vh);
    const ImVec2 sz(vw * scale, vh * scale);
    const ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - sz.x) * 0.5f, cursor.y + (avail.y - sz.y) * 0.5f));
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(videoDecoder().texture())), sz);
}

void drawVideoTile(const ClientArena& arena, ImVec2 size) {
    theme::beginCard("videotile", size, theme::card());
    ImGui::TextColored(theme::primary(), "Live View");
    ImGui::SameLine();
    const auto& v = arena.video;
    if (v.haveStatus) {
        ImGui::TextColored(v.connected ? theme::success() : theme::warning(), "%s  %s  %0.0f fps  drop %llu",
                           v.connected ? "up" : "down", v.mode.c_str(), v.fps,
                           static_cast<unsigned long long>(v.dropped));
    } else {
        ImGui::TextColored(theme::mutedFg(), "idle");
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    drawVideoImage(avail);
    theme::endCard();
}

}  // namespace photon::ui
