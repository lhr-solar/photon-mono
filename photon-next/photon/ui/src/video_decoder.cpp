#include "photon_ui/video.hpp"
#include "photon_ui/video_release.hpp"

#include "photon/video_framing.hpp"
#include "photon_ui/theme.hpp"

#include "photon_ui/gl_api.hpp"

#include "imgui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace photon::ui {
namespace {

std::size_t gLivePicsFed = 0;
std::size_t gLiveSkipPics = 0;

void resetLiveGopFeed() {
    gLivePicsFed = 0;
    gLiveSkipPics = 0;
}

#ifdef __EMSCRIPTEN__
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
            auParts: [],
            auTs: -1,
            auKey: false,
            lastKeyTs: -1,
            lastKeyLen: 0,
            lastVclAt: 0,
            heldKey: false,
            needKey: false,
            spsCodec: "",
        };
    }
    const s = Module._photonVideo;
    if (s.needKey === undefined) {
        s.needKey = false;
    }
    if (s.spsCodec === undefined) {
        s.spsCodec = "";
    }
    s.startLen = function(bytes) {
        if (bytes.length >= 4 && bytes[0] === 0 && bytes[1] === 0 && bytes[2] === 0 && bytes[3] === 1) {
            return 4;
        }
        if (bytes.length >= 3 && bytes[0] === 0 && bytes[1] === 0 && bytes[2] === 1) {
            return 3;
        }
        return 0;
    };
    s.firstMb = function(bytes) {
        const i = s.startLen(bytes) + 1;
        if (i >= bytes.length) {
            return 0;
        }
        let bit = 0;
        const getBit = function() {
            const byte = bytes[i + (bit >> 3)];
            if (byte === undefined) {
                return 0;
            }
            const v = (byte >> (7 - (bit & 7))) & 1;
            bit++;
            return v;
        };
        let zeros = 0;
        while (zeros < 24 && getBit() === 0) {
            zeros++;
        }
        if (zeros >= 24) {
            return 0;
        }
        let val = 1;
        for (let n = 0; n < zeros; n++) {
            val = (val << 1) | getBit();
        }
        return val - 1;
    };
    s.forEachNal = function(bytes) {
        const out = [];
        if (!bytes || !bytes.length) {
            return out;
        }
        if (s.startLen(bytes) === 0) {
            out.push(bytes);
            return out;
        }
        let pos = 0;
        while (pos < bytes.length) {
            const here = s.startLen(bytes.subarray(pos));
            if (here === 0) {
                break;
            }
            let next = bytes.length;
            for (let i = pos + here; i < bytes.length; i++) {
                if (s.startLen(bytes.subarray(i)) !== 0) {
                    next = i;
                    break;
                }
            }
            out.push(bytes.subarray(pos, next));
            pos = next;
        }
        return out;
    };
    s.concatParts = function() {
        if (!s.auParts.length) {
            return null;
        }
        if (s.auParts.length === 1) {
            return s.auParts[0];
        }
        let n = 0;
        for (const p of s.auParts) {
            n += p.length;
        }
        const data = new Uint8Array(n);
        let o = 0;
        for (const p of s.auParts) {
            data.set(p, o);
            o += p.length;
        }
        return data;
    };
    s.dropAU = function() {
        s.auParts = [];
        s.auKey = false;
        s.auTs = -1;
    };
    s.flushAU = function(forceOutput) {
        if (!s.auParts || !s.auParts.length || !s.configured || !s.decoder) {
            s.dropAU();
            return;
        }
        const data = s.concatParts();
        const isKey = s.auKey;
        const ts = s.auTs;
        s.dropAU();
        if (!data) {
            return;
        }
        // Chrome rejects type:'delta' after configure() or flush() until the
        // next IDR. Catalog Play used to flush the key then feed P-frames.
        if (!isKey && s.needKey) {
            return;
        }
        try {
            s.decoder.decode(new EncodedVideoChunk({
                type: isKey ? 'key' : 'delta',
                timestamp: Math.round(ts * 1000),
                data: data,
            }));
            s.heldKey = !!isKey;
            if (isKey) {
                s.needKey = false;
            }
            // Still GOP only: Chrome holds the IDR until flush() or another
            // chunk. Play/live must not flush() after a key or the next
            // P-frame has no reference and the picture goes blocky.
            if (forceOutput && isKey && s.decoder.state === 'configured') {
                s.heldKey = false;
                s.needKey = true;
                s.decoder.flush().catch(function(err) {
                    s.error = err && err.message ? String(err.message) : String(err);
                    s.needKey = true;
                });
            }
        } catch (e) {
            s.error = e && e.message ? String(e.message) : String(e);
            s.needKey = true;
        }
    };
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
    s.auParts = [];
    s.auTs = -1;
    s.auKey = false;
    s.lastKeyTs = -1;
    s.lastKeyLen = 0;
    s.lastVclAt = 0;
    s.heldKey = false;
    s.needKey = false;
    s.spsCodec = "";
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

    const bytes = HEAPU8.slice(ptr, ptr + len);

    const nalType = function(part) {
        const i = s.startLen(part);
        return i < part.length ? (part[i] & 0x1f) : 0;
    };
    const codecFromSps = function(part) {
        const i = s.startLen(part) + 1;
        if (i + 2 >= part.length) {
            return 'avc1.42E01E';
        }
        const hex = function(n) { return ('0' + (n & 0xff).toString(16)).slice(-2).toUpperCase(); };
        return 'avc1.' + hex(part[i]) + hex(part[i + 1]) + hex(part[i + 2]);
    };
    const ensureDecoder = function() {
        if (s.decoder) {
            return true;
        }
        try {
            s.decoder = new VideoDecoder({
                output: function(frame) {
                    s.pending.push(frame);
                    // Keep a short in-order queue. Collapsing to the newest
                    // frame made a GOP submitted in one tick play as one still.
                    while (s.pending.length > 12) {
                        try {
                            s.pending.shift().close();
                        } catch (e) {
                        }
                    }
                },
                error: function(err) {
                    s.error = err && err.message ? String(err.message) : String(err);
                    s.configured = false;
                    s.needKey = true;
                    if (s.dropAU) {
                        s.dropAU();
                    }
                    try {
                        if (s.decoder) {
                            s.decoder.close();
                        }
                    } catch (e) {
                    }
                    s.decoder = null;
                },
            });
        } catch (e) {
            s.error = e && e.message ? String(e.message) : String(e);
            return false;
        }
        return true;
    };

    if (!s.forEachNal) {
        s.forEachNal = function(buf) {
            return buf && buf.length ? [buf] : [];
        };
    }
    // Locate/live often pack AUD+SPS+PPS+IDR in one buffer. The first NAL is
    // then 9 (AUD) or 7 (SPS); treating the whole AU as one NAL drops the IDR.
    const parts = s.forEachNal(bytes);
    for (let pi = 0; pi < parts.length; pi++) {
        const nal = parts[pi];
        const t = nalType(nal);
        if (t !== 1 && t !== 5 && t !== 7 && t !== 8) {
            continue;
        }
        if (t === 1 && nal.length < 24) {
            continue;
        }

        if (t === 7 || t === 8) {
            // Car cameras restamp SPS/PPS on every access unit. Closing the
            // decoder on each copy cancels the in-flight IDR before output().
            // Reconfigure only when profile/constraints/level actually change.
            if (t === 7) {
                const copy = nal.slice();
                const codec = codecFromSps(copy);
                const changed = s.spsCodec !== codec;
                s.sps = copy;
                s.spsCodec = codec;
                if (changed) {
                    s.configured = false;
                    s.needKey = true;
                    if (s.dropAU) {
                        s.dropAU();
                    }
                    if (s.decoder) {
                        try {
                            s.decoder.close();
                        } catch (e) {
                        }
                        s.decoder = null;
                    }
                }
            } else {
                s.pps = nal.slice();
            }
            continue;
        }

        if (!ensureDecoder()) {
            return;
        }

        // Skip a resent IDR only after a picture has actually landed. Recording
        // lastKeyTs before configure() used to drop the real IDR forever.
        if (t === 5 && s.lastKeyTs === tsMs && s.lastKeyLen === nal.length) {
            continue;
        }

        if (!s.configured && s.sps && t === 5) {
            try {
                s.decoder.configure({
                    codec: codecFromSps(s.sps),
                    optimizeForLatency: true,
                    hardwareAcceleration: 'prefer-software',
                });
                s.configured = true;
                s.needKey = true;
                s.error = "";
            } catch (e) {
                try {
                    s.decoder.configure({codec: codecFromSps(s.sps), optimizeForLatency: true});
                    s.configured = true;
                    s.needKey = true;
                    s.error = "";
                } catch (e2) {
                    s.error = e2 && e2.message ? String(e2.message) : String(e2);
                    return;
                }
            }
        }
        if (!s.configured || !s.decoder) {
            continue;
        }

        if (!s.auParts) {
            s.auParts = [];
            s.auTs = -1;
            s.auKey = false;
        }
        // Same timestamp is not enough: locate stamps every NAL in a GOP with
        // row.startMs, and the car sends several slices per picture. A new
        // picture starts at first_mb_in_slice == 0. Splitting there keeps
        // WebCodecs from decoding a half-frame (that shows as green).
        if (s.auParts.length && (s.auTs !== tsMs || s.firstMb(nal) === 0)) {
            s.flushAU(false);
        }
        let piece = nal;
        if (t === 5 && s.sps && s.pps && s.auParts.length === 0) {
            piece = new Uint8Array(s.sps.length + s.pps.length + nal.length);
            piece.set(s.sps, 0);
            piece.set(s.pps, s.sps.length);
            piece.set(nal, s.sps.length + s.pps.length);
        }
        s.auTs = tsMs;
        s.auKey = s.auKey || (t === 5);
        s.auParts.push(piece);
        if (t === 5) {
            s.lastKeyTs = tsMs;
            s.lastKeyLen = nal.length;
        }
        s.lastVclAt = (typeof performance !== 'undefined') ? performance.now() : Date.now();
    }
});

EM_JS(void, photon_video_end_au, (int force), {
    const s = Module._photonVideo;
    if (!s) {
        return;
    }
    if (s.flushAU && s.auParts && s.auParts.length) {
        s.flushAU(!!force);
        return;
    }
    // Only a still-image request may drain the codec. A short live gap is
    // normal (including locate polling); flush() makes the next decode
    // require a keyframe and rejects the arriving delta frames.
    if (!force) {
        return;
    }
    if (!s.configured || !s.decoder || s.decoder.state !== 'configured') {
        return;
    }
    if (s.pending && s.pending.length) {
        return;
    }
    s.heldKey = false;
    s.needKey = true;
    s.decoder.flush().catch(function(err) {
        s.error = err && err.message ? String(err.message) : String(err);
        s.needKey = true;
    });
});

EM_JS(int, photon_video_upload, (int glTex, double cursorMs), {
    const s = Module._photonVideo;
    if (!s || s.pending.length === 0) {
        return 0;
    }
    let frame = null;
    let keep = false;
    if (cursorMs > 0) {
        // EncodedVideoChunk timestamp is microseconds (auTs * 1000).
        const curUs = cursorMs * 1000;
        let best = -1;
        for (let i = 0; i < s.pending.length; i++) {
            if (s.pending[i].timestamp <= curUs) {
                best = i;
            }
        }
        if (best < 0) {
            return 0;
        }
        for (let i = 0; i < best; i++) {
            try { s.pending[i].close(); } catch (e) {}
        }
        s.pending = s.pending.slice(best);
        frame = s.pending[0];
        keep = true;
    } else {
        frame = s.pending.shift();
    }
    s.width = frame.displayWidth | 0;
    s.height = frame.displayHeight | 0;
    const tex = (typeof GL !== 'undefined' && GL.textures) ? GL.textures[glTex] : null;
    if (!tex || typeof GLctx === 'undefined') {
        if (!keep) {
            s.pending.unshift(frame);
        }
        return 0;
    }
    GLctx.bindTexture(GLctx.TEXTURE_2D, tex);
    try {
        GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA, GLctx.UNSIGNED_BYTE, frame);
    } catch (e) {
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
    if (!keep) {
        try { frame.close(); } catch (e) {}
    }
    return 1;
});
#else
extern "C" {
int photon_video_supported();
void photon_video_init();
void photon_video_reset();
void photon_video_copy_error(char* ptr, int cap);
int photon_video_dim(int which);
void photon_video_feed(const std::uint8_t* ptr, int len, double tsMs, int flags);
void photon_video_end_au(int force);
int photon_video_upload(unsigned glTex, double cursorMs);
}
#endif

VideoDecoder gDecoder;

}  // namespace

VideoDecoder& videoDecoder() { return gDecoder; }

VideoDecoder::~VideoDecoder() {
#ifndef __EMSCRIPTEN__
    if (!photonGlContextLive()) {
        tex_ = 0;
        return;
    }
#endif
    reset();
    if (tex_ != 0) {
        GLuint tex = tex_;
        glDeleteTextures(1, &tex);
        tex_ = 0;
    }
}

bool VideoDecoder::supported() const { return photon_video_supported() != 0; }

bool VideoDecoder::hasVideoAt(std::uint64_t videoMs) const {
    if (videoMs == 0 || ring_.nals().empty()) {
        return false;
    }
    if (videoMs + 80 < ring_.oldestMs()) {
        return false;
    }
    return ring_.keyframeAtOrBefore(videoMs) >= 0 && videoMs <= ring_.newestMs() + 80;
}

void VideoDecoder::ensureTexture() {
    if (!supported() || tex_ != 0) {
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

void VideoDecoder::submit(const vidx::BufferedNal& buffered) {
    if (video::nalIsTinyPslice(buffered.nal)) {
        return;
    }
#ifdef __EMSCRIPTEN__
    photon_video_feed(reinterpret_cast<int>(buffered.nal.data()), static_cast<int>(buffered.nal.size()),
                      static_cast<double>(buffered.videoMs), static_cast<int>(buffered.flags));
    photon_video_copy_error(reinterpret_cast<int>(error_), static_cast<int>(sizeof(error_)));
#else
    photon_video_feed(buffered.nal.data(), static_cast<int>(buffered.nal.size()),
                      static_cast<double>(buffered.videoMs), static_cast<int>(buffered.flags));
    photon_video_copy_error(error_, static_cast<int>(sizeof(error_)));
#endif
    if (buffered.seq > releasedSeq_) {
        releasedSeq_ = buffered.seq;
    }
    releasedMs_ = buffered.videoMs;
}

void VideoDecoder::pushToRing(std::uint64_t timestampMs, std::uint8_t flags, std::span<const std::uint8_t> nal) {
    if (mode_ == vidx::PlaybackMode::Live && liveIgnoreOlderThanMs_ != 0 &&
        timestampMs < liveIgnoreOlderThanMs_) {
        return;
    }
    if (mode_ == vidx::PlaybackMode::Live && liveIgnoreOlderThanMs_ != 0 &&
        timestampMs >= liveIgnoreOlderThanMs_) {
        liveIgnoreOlderThanMs_ = 0;
    }
    if (mode_ == vidx::PlaybackMode::Live) {
        const bool key = (flags & proto::kVideoFlagKeyframe) != 0;
        const bool cfg = (flags & proto::kVideoFlagConfig) != 0;
        const bool pic = !cfg;
        if (key) {
            if (lastLiveKeyMs_ != 0 && timestampMs < lastLiveKeyMs_) {
                // Older GOP from a stale pointer. Drop the whole batch.
                skipLiveGop_ = true;
                return;
            }
            if (lastLiveKeyMs_ != 0 && timestampMs == lastLiveKeyMs_) {
                // Same live GOP fetched again (it grew). Skip the IDR and
                // pictures we already submitted; keep the new tail.
                skipLiveGop_ = false;
                gLiveSkipPics = gLivePicsFed;
                if (gLiveSkipPics > 0) {
                    --gLiveSkipPics;
                    return;
                }
            } else {
                skipLiveGop_ = false;
                lastLiveKeyMs_ = timestampMs;
                gLivePicsFed = 0;
                gLiveSkipPics = 0;
            }
        } else if (skipLiveGop_ && pic) {
            return;
        }
        if (pic && gLiveSkipPics > 0) {
            --gLiveSkipPics;
            return;
        }
        if (pic) {
            ++gLivePicsFed;
        }
    }
    // Adjacent locate replies overlap by an entry GOP. Keep its existing
    // bytes rather than appending and replaying that GOP a second time.
    if (mode_ != vidx::PlaybackMode::Live && batchFloorMs_ != 0 && timestampMs <= batchFloorMs_) {
        return;
    }
    // Locate used to stamp every NAL with GOP startMs. Cursor Play then either
    // held P-frames forever or dumped them in one tick (one last still).
    std::uint64_t stamp = timestampMs;
    if ((flags & proto::kVideoFlagConfig) != 0) {
        // Live pictures ride a synthetic 33 ms timeline, so a config NAL that
        // kept its wall-clock stamp would sort after the pictures it belongs
        // in front of and stall the paced release on that NAL.
        if (mode_ == vidx::PlaybackMode::Live && haveVcl_) {
            stamp = nextPicMs_;
        }
    } else {
        const bool newPic = video::nalStartsPicture(nal) || !haveVcl_;
        if (newPic) {
            if (haveVcl_ && timestampMs == lastRawMs_) {
                nextPicMs_ += 33;
            } else {
                nextPicMs_ = timestampMs;
                lastRawMs_ = timestampMs;
            }
            haveVcl_ = true;
        }
        stamp = nextPicMs_;
    }
    if (playback_.replayWindowOn() && (flags & proto::kVideoFlagConfig) == 0 &&
        !playback_.videoInReplayWindow(stamp)) {
        return;
    }
    ring_.push(stamp, flags, nal);
    // Live used to submit here. One locate reply is a whole ~1 s GOP, so that
    // fed WebCodecs 30 pictures inside one UI frame and the upload path showed
    // only the newest: live played at the locate rate. releaseLivePaced hands
    // them over on a clock instead.
}

void VideoDecoder::feed(std::uint64_t timestampMs, std::uint8_t flags, std::span<const std::uint8_t> nal) {
    if (!supported()) {
        return;
    }
    if (!inited_) {
        photon_video_init();
        inited_ = true;
    }
    ensureTexture();
    if (nal.empty()) {
        return;
    }
    bool split = false;
    video::forEachAnnexB(nal, [&](std::span<const std::uint8_t> part) {
        const int t = video::annexBNalType(part);
        if (t != 1 && t != 5 && t != 7 && t != 8) {
            return;
        }
        if (video::nalIsTinyPslice(part)) {
            return;
        }
        split = true;
        pushToRing(timestampMs, video::flagsForNalType(t), part);
    });
    if (!split && !video::nalIsTinyPslice(nal)) {
        pushToRing(timestampMs, flags, nal);
    }
}

void VideoDecoder::releaseThrough(std::uint64_t videoMs) {
    releaseVideoThrough(ring_, releasedSeq_, videoMs,
                        [&](const vidx::BufferedNal& buffered) { submit(buffered); });
}

void VideoDecoder::reenterAt(std::uint64_t keyframeVideoMs, std::uint64_t feedUpToMs) {
    const std::ptrdiff_t idx = ring_.keyframeAtOrBefore(keyframeVideoMs);
    if (idx < 0) {
        // Nothing resident to start on. Asking WebCodecs to decode a delta
        // frame with no reference produces a decoder error, not a picture.
        return;
    }
    const auto start = static_cast<std::size_t>(idx);
    photon_video_reset();
    // Locate reply carries SPS/PPS even when the GOP byte range does not.
    // Feed those first so WebCodecs can configure before the IDR.
    const std::uint64_t cfgTs = ring_.nals()[start].videoMs;
    auto submitBlob = [&](const std::vector<std::uint8_t>& blob) {
        if (blob.empty()) {
            return;
        }
        vidx::BufferedNal cfg;
        cfg.videoMs = cfgTs;
        cfg.flags = proto::kVideoFlagConfig;
        cfg.nal = blob;
        submit(cfg);
    };
    submitBlob(playback_.sps());
    submitBlob(playback_.pps());
    std::size_t from = start;
    while (from > 0 && (ring_.nals()[from - 1].flags & proto::kVideoFlagConfig) != 0) {
        --from;
    }
    releasedSeq_ = 0;
    const std::uint64_t keyTs = ring_.nals()[start].videoMs;
    const bool keyOnly = feedUpToMs <= keyTs;
    for (std::size_t i = from; i < ring_.nals().size(); ++i) {
        const vidx::BufferedNal& buffered = ring_.nals()[i];
        if (i > start) {
            if (buffered.videoMs > feedUpToMs) {
                break;
            }
            if (keyOnly && (buffered.flags & proto::kVideoFlagConfig) == 0 &&
                (buffered.flags & proto::kVideoFlagKeyframe) == 0) {
                break;
            }
        }
        submit(buffered);
    }
    // Submit the current AU. Do not codec-flush: Chrome then requires the
    // next chunk to be a key, and Live/Play immediately feed P-frames.
    photon_video_end_au(0);
}

namespace {
constexpr char kNoRunVideo[] = "This run has no recorded camera video";
}  // namespace

void VideoDecoder::releaseLivePaced(double nowMs) {
    if (ring_.nals().empty()) {
        return;
    }
    const std::uint64_t newest = ring_.newestMs();
    const LivePacing plan = paceLiveRelease(liveReleaseMs_, newest, nowMs - liveWallMs_);
    if (plan.reenter) {
        const std::ptrdiff_t kf = ring_.keyframeAtOrBefore(newest);
        if (kf < 0) {
            return;
        }
        const std::uint64_t kfMs = ring_.nals()[static_cast<std::size_t>(kf)].videoMs;
        // Feed the keyframe only. Its P-frames are the buffer the clock plays
        // out; feeding them here is the dump-the-GOP behaviour being fixed.
        reenterAt(kfMs, kfMs);
        liveReleaseMs_ = kfMs;
        liveWallMs_ = nowMs;
        return;
    }
    liveWallMs_ = nowMs;
    liveReleaseMs_ = plan.releaseMs;
    releaseThrough(static_cast<std::uint64_t>(liveReleaseMs_));
}

void VideoDecoder::syncToCursor(const ClientArena& arena) {
    if (!supported()) {
        return;
    }
    if (!inited_) {
        photon_video_init();
        inited_ = true;
    }

    const std::uint64_t cursorMs = arena.videoCursorMs();
    const std::int64_t clockOff =
        arena.video.clockOffsetUser ? arena.video.clockOffsetMs : playback_.clockOffsetMs();
    if (arena.video.clockOffsetUser && playback_.clockOffsetMs() != clockOff) {
        playback_.setClockOffsetMs(clockOff);
    }
    const std::uint64_t videoMs = vidx::videoMsForCursor(cursorMs, clockOff);
    const bool inCoverageGap =
        arena.catalogPinned() && !playback_.coverage().empty() && !playback_.covered(cursorMs);
    const bool pastRecordedEnd =
        pastVideoEnd_ && arena.catalogPinned() && videoEndMs_ != 0 && videoMs > videoEndMs_;
    if (pastRecordedEnd) {
        havePendingLocate_ = false;
        std::snprintf(error_, sizeof(error_), "%s", kNoRunVideo);
        return;
    }
    if (inCoverageGap) {
        // Clipped coverage can miss the playhead even when video exists at
        // this time in the run. Only treat it as a hole when some coverage
        // actually sits next to the cursor.
        bool coverageNear = false;
        for (const auto& c : playback_.coverage()) {
            if (c.endMs + vidx::kReplayHoldMs >= videoMs && c.startMs <= videoMs + vidx::kReplayHoldMs) {
                coverageNear = true;
                break;
            }
        }
        if (!coverageNear) {
            if (!havePendingLocate_) {
                pendingLocateMs_ = videoMs == 0 ? 1 : videoMs;
                havePendingLocate_ = true;
            }
        } else {
            havePendingLocate_ = false;
            if (ring_.nals().empty()) {
                const std::uint64_t minV = vidx::videoMsForCursor(arena.timeline().minTimeMs, clockOff);
                const std::uint64_t maxV = arena.timeline().maxTimeMs == 0
                                               ? 0
                                               : vidx::videoMsForCursor(arena.timeline().maxTimeMs, clockOff);
                const std::uint64_t snapV = vidx::firstCoveredVideoMs(playback_.coverage(), minV, maxV);
                if (snapV != 0) {
                    pendingLocateMs_ = snapV;
                    havePendingLocate_ = true;
                }
            }
            return;
        }
    }
    if (pastVideoEnd_ && arena.catalogPinned() && !playback_.coverage().empty() &&
        playback_.covered(cursorMs)) {
        pastVideoEnd_ = false;
        error_[0] = '\0';
    }
    vidx::PlaybackMode next = vidx::PlaybackMode::Idle;
    const bool liveCache = arena.video.subscribed && arena.video.mode == "live";
    const bool followClient = arena.catalogPinned() || arena.hasPreviewCursor() ||
                              arena.videoFollowMs() != 0;
    // A catalog pin / rail pointer is a historical time. Live subscribe would
    // keep showing today's camera (or sit in Live and never video_locate).
    if (followClient) {
        if (arena.hasPreviewCursor()) {
            next = vidx::PlaybackMode::Scrubbing;
        } else {
            next = cursorMs != 0 ? vidx::PlaybackMode::Committed : vidx::PlaybackMode::Idle;
        }
    } else if (liveCache) {
        next = vidx::PlaybackMode::Live;
    } else if (cursorMs != 0) {
        next = vidx::PlaybackMode::Committed;
    } else {
        next = vidx::PlaybackMode::Live;
    }
    const bool modeChanged = next != mode_;
    mode_ = next;
    if (modeChanged) {
        ring_.setBudget(next == vidx::PlaybackMode::Live || next == vidx::PlaybackMode::Idle ? vidx::kLiveCacheBudget
                                                                                            : vidx::kReplayCacheBudget);
    }

    auto forceCachedStill = [&]() {
        if (haveFrame_ || ring_.nals().empty() || mode_ == vidx::PlaybackMode::Idle) {
            return;
        }
        const std::uint64_t want =
            mode_ == vidx::PlaybackMode::Live ? ring_.newestMs() : videoMs;
        // Cursor time can sit before the cached GOP (clock offset, locate 0).
        // The ring still has a picture; decode that instead of staying blank.
        std::ptrdiff_t kf = ring_.keyframeAtOrBefore(want);
        if (kf < 0) {
            kf = ring_.keyframeAtOrBefore(ring_.newestMs());
        }
        if (kf < 0) {
            return;
        }
        const std::uint64_t kfMs = ring_.nals()[static_cast<std::size_t>(kf)].videoMs;
        bool haveDelta = false;
        for (std::size_t i = static_cast<std::size_t>(kf) + 1; i < ring_.nals().size(); ++i) {
            const std::uint8_t flags = ring_.nals()[i].flags;
            if ((flags & proto::kVideoFlagKeyframe) == 0 && (flags & proto::kVideoFlagConfig) == 0) {
                haveDelta = true;
                break;
            }
        }
        // reenterAt resets the decoder. Doing that after the GOP was already
        // submitted this frame cancelled the in-flight still. Live must also
        // feed P-frames after the IDR or the tile never leaves the first
        // picture.
        if (releasedSeq_ == 0) {
            reenterAt(kfMs, mode_ == vidx::PlaybackMode::Live ? kfMs : want);
        }
        if (mode_ == vidx::PlaybackMode::Live) {
            return;
        }
        if (haveDelta) {
            photon_video_end_au(0);
            return;
        }
        if (forcedStillTries_ >= 8) {
            return;
        }
        ++forcedStillTries_;
        photon_video_end_au(1);
    };

    if (mode_ == vidx::PlaybackMode::Idle) {
        return;
    }
    if (mode_ == vidx::PlaybackMode::Live) {
        if (modeChanged) {
            // Jump to the newest GOP. Replaying the whole ring makes Live lag
            // by however many seconds were buffered during a down/live flap.
            liveReleaseMs_ = 0;
        }
        releaseLivePaced(ImGui::GetTime() * 1000.0);
        forceCachedStill();
        // Always probe t=0 (newest GOP). Locating at the previous maxMs
        // stuck on that GOP: locate(endMs) is "at or before", so the next
        // camera GOP never became the entry. flushVideoLocate rate-limits
        // the RPC; skip-same-IDR keeps a growing GOP from replaying.
        if (!havePendingLocate_) {
            pendingLocateMs_ = 0;
            havePendingLocate_ = true;
        }
        return;
    }

    const vidx::CursorPlan plan = playback_.update(cursorMs, mode_);
    if (plan.requestLocate) {
        pendingLocateMs_ = plan.locateAtMs;
        havePendingLocate_ = true;
    } else if (!havePendingLocate_ && !ring_.nals().empty() &&
               (mode_ == vidx::PlaybackMode::Committed || mode_ == vidx::PlaybackMode::Scrubbing)) {
        const std::uint64_t videoMs = vidx::videoMsForCursor(cursorMs, playback_.clockOffsetMs());
        // Pointer rows are not bytes. A fat prefetch table used to suppress
        // locate while the ring still held one GOP, so Play froze on a still.
        const bool outsideRing = videoMs + 50 < ring_.oldestMs() || videoMs > ring_.newestMs() + 200;
        const bool ringRunningOut =
            mode_ == vidx::PlaybackMode::Committed && videoMs + vidx::kLocateLeadMs >= ring_.newestMs();
        const bool beforeEnd = videoEndMs_ == 0 || ring_.newestMs() + 100 < videoEndMs_;
        if (beforeEnd && (outsideRing || ringRunningOut)) {
            pendingLocateMs_ = outsideRing ? (videoMs == 0 ? 1 : videoMs) : ring_.newestMs() + 1;
            havePendingLocate_ = true;
        }
    }
    if (!plan.haveTarget) {
        // No pointer covers the cursor. Fall back to the ring alone, which is
        // what makes a small scrub work before any pointer has been fetched.
        const std::uint64_t videoMs = vidx::videoMsForCursor(cursorMs, playback_.clockOffsetMs());
        if (ring_.keyframeAtOrBefore(videoMs) >= 0 &&
            (releasedSeq_ == 0 || videoMs < lastCursorVideoMs_)) {
            reenterAt(videoMs, videoMs);
        } else {
            releaseThrough(videoMs);
        }
        lastCursorVideoMs_ = videoMs;
        forceCachedStill();
        return;
    }
    // Crossing a GOP boundary during forward playback is not a seek.
    // Reset/re-feed only on initial entry or an actual backwards seek.
    if (releasedSeq_ == 0 || plan.feedUpToMs < lastCursorVideoMs_) {
        reenterAt(plan.target.startMs, plan.feedUpToMs);
    } else {
        releaseThrough(plan.feedUpToMs);
    }
    lastCursorVideoMs_ = plan.feedUpToMs;
    forceCachedStill();
}

bool VideoDecoder::applyLocateReply(std::span<const std::uint8_t> payload) {
    vidx::LocateReply reply;
    if (!vidx::decodeLocateReply(payload, reply)) {
        std::snprintf(error_, sizeof(error_), "video_locate reply was malformed");
        return false;
    }
    playback_.applyLocate(reply);
    if (mode_ == vidx::PlaybackMode::Live) {
        liveIgnoreOlderThanMs_ = 0;
    }
    if (playback_.replayWindowOn() && reply.found && playback_.rows().empty()) {
        const std::uint64_t snap = playback_.firstReplayCoveredMs();
        if (snap != 0 && snap != pendingLocateMs_) {
            pendingLocateMs_ = snap;
            havePendingLocate_ = true;
            return true;
        }
        havePendingLocate_ = false;
        if (!haveFrame_) {
            std::snprintf(error_, sizeof(error_), "%s", kNoRunVideo);
        }
        return true;
    }
    // A locate is a replay batch unless a live subscription explicitly owns
    // it. Catalog pin calls stopVideo/reset before requesting its first batch.
    if (mode_ != vidx::PlaybackMode::Live) {
        if (!ring_.nals().empty() && pendingLocateMs_ < ring_.oldestMs()) {
            ring_.clear();
            releasedSeq_ = 0;
            lastCursorVideoMs_ = 0;
            haveVcl_ = false;
        }
        batchFloorMs_ = ring_.nals().empty() ? 0 : ring_.newestMs();
    }
    if (reply.maxMs != 0) {
        videoEndMs_ = reply.maxMs;
    }
    // Do not chase reply.maxMs on Live. t=0 already picks the newest GOP.
    // If that GOP ended more than 2.5 s behind the table tail (camera idle,
    // index ahead of bytes), the old chase cleared the ring, set
    // liveIgnoreOlderThanMs_ past the GOP we just fetched, dropped every NAL,
    // then idle-skipped the next locate — Live View sat on Buffering.
    if (!reply.found) {
        // A hole or a dropped locate is not "this run has no camera". Keep
        // the last GOP so scrubbing back still has a picture.
        if (reply.maxMs != 0 && pendingLocateMs_ > reply.maxMs) {
            pastVideoEnd_ = true;
            std::snprintf(error_, sizeof(error_), "%s", kNoRunVideo);
        }
    } else {
        pastVideoEnd_ = false;
        if (error_[0] != '\0' && std::strcmp(error_, kNoRunVideo) == 0) {
            error_[0] = '\0';
        }
    }
    havePendingLocate_ = false;
    return true;
}

void VideoDecoder::setClockOffsetMs(std::int64_t offsetMs) {
    playback_.setClockOffsetMs(offsetMs);
}

void VideoDecoder::setReplayWindow(std::uint64_t minCursorMs, std::uint64_t maxCursorMs) {
    playback_.setReplayWindow(minCursorMs, maxCursorMs);
}

void VideoDecoder::clearReplayWindow() {
    playback_.clearReplayWindow();
}

bool VideoDecoder::pendingLocate(std::uint64_t& atMs) const {
    if (!havePendingLocate_) {
        return false;
    }
    atMs = pendingLocateMs_;
    return true;
}

void VideoDecoder::requestLocate(std::uint64_t atMs) {
    pendingLocateMs_ = atMs;
    havePendingLocate_ = true;
    // A timeline seek must re-enter the GOP at the playhead, not keep
    // releasing forward from the previous still.
    lastCursorVideoMs_ = 0;
    if (!ring_.nals().empty() && atMs != 0 &&
        (atMs + 50 < ring_.oldestMs() || atMs > ring_.newestMs() + 2000)) {
        ring_.clear();
        releasedSeq_ = 0;
        haveVcl_ = false;
        batchFloorMs_ = 0;
        haveFrame_ = false;
    }
}

void VideoDecoder::cancelPendingLocate() {
    havePendingLocate_ = false;
    playback_.cancelPendingLocate();
}

void VideoDecoder::resumeLive() {
    pastVideoEnd_ = false;
    videoEndMs_ = 0;
    liveIgnoreOlderThanMs_ = 0;
    lastLiveKeyMs_ = 0;
    liveReleaseMs_ = 0;
    skipLiveGop_ = false;
    resetLiveGopFeed();
    havePendingLocate_ = false;
    pendingLocateMs_ = 0;
    playback_.cancelPendingLocate();
    if (error_[0] != '\0' && std::strcmp(error_, kNoRunVideo) == 0) {
        error_[0] = '\0';
    }
}

void VideoDecoder::poll() {
    if (!supported()) {
        return;
    }
    if (!inited_) {
        photon_video_init();
        inited_ = true;
    }
    ensureTexture();
    const double cursorMs =
        (mode_ == vidx::PlaybackMode::Live || lastCursorVideoMs_ == 0)
            ? 0.0
            : static_cast<double>(lastCursorVideoMs_);
#ifdef __EMSCRIPTEN__
    photon_video_end_au(0);
    if (photon_video_upload(static_cast<int>(tex_), cursorMs) != 0) {
        haveFrame_ = true;
        width_ = photon_video_dim(0);
        height_ = photon_video_dim(1);
        forcedStillTries_ = 0;
    }
    photon_video_copy_error(reinterpret_cast<int>(error_), static_cast<int>(sizeof(error_)));
#else
    photon_video_end_au(0);
    if (photon_video_upload(tex_, cursorMs) != 0) {
        haveFrame_ = true;
        width_ = photon_video_dim(0);
        height_ = photon_video_dim(1);
        forcedStillTries_ = 0;
    }
    photon_video_copy_error(error_, static_cast<int>(sizeof(error_)));
#endif
    if (!haveFrame_ && std::strstr(error_, "key frame") != nullptr) {
        releasedSeq_ = 0;
        lastCursorVideoMs_ = 0;
        liveReleaseMs_ = 0;
        photon_video_reset();
        error_[0] = '\0';
    }
    if (pastVideoEnd_ && mode_ != vidx::PlaybackMode::Live) {
        haveFrame_ = false;
        std::snprintf(error_, sizeof(error_), "%s", kNoRunVideo);
    }
}

void VideoDecoder::reset() {
    if (inited_) {
        photon_video_reset();
    }
    haveFrame_ = false;
    width_ = 0;
    height_ = 0;
    error_[0] = '\0';
    ring_.clear();
    playback_.clear();
    releasedSeq_ = 0;
    releasedMs_ = 0;
    havePendingLocate_ = false;
    pendingLocateMs_ = 0;
    lastRawMs_ = 0;
    nextPicMs_ = 0;
    haveVcl_ = false;
    mode_ = vidx::PlaybackMode::Idle;
    lastCursorVideoMs_ = 0;
    batchFloorMs_ = 0;
    pastVideoEnd_ = false;
    videoEndMs_ = 0;
    liveIgnoreOlderThanMs_ = 0;
    lastLiveKeyMs_ = 0;
    skipLiveGop_ = false;
    resetLiveGopFeed();
    forcedStillTries_ = 0;
    liveReleaseMs_ = 0;
    liveWallMs_ = 0.0;
}

static void strokeBufferingSpinner(ImVec2 center, float r, const char* label, ImU32 labelCol) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = static_cast<float>(ImGui::GetTime());
    const float a0 = t * 4.2f;
    const float a1 = a0 + 4.4f;
    dl->PathClear();
    dl->PathArcTo(center, r, a0, a1, 24);
    dl->PathStroke(theme::u32(theme::primary()), 0, std::max(2.5f, r * 0.16f));
    if (label != nullptr && label[0] != '\0') {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y + r + 10.0f), labelCol, label);
    }
}

void drawVideoBuffering(ImVec2 avail) {
    ImGui::Dummy(avail);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 center(p0.x + avail.x * 0.5f, p0.y + avail.y * 0.5f - 8.0f);
    const float r = std::clamp(std::min(avail.x, avail.y) * 0.12f, 14.0f, 36.0f);
    const char* label = videoDecoder().error()[0] != '\0' ? videoDecoder().error() : "Buffering";
    strokeBufferingSpinner(center, r, label,
                           theme::u32(videoDecoder().error()[0] != '\0' ? theme::danger() : theme::mutedFg()));
}

void drawVideoImage(ImVec2 avail) {
    if (!videoDecoder().supported()) {
        ImGui::TextColored(theme::warning(), "Video decoder unavailable");
        return;
    }
    videoDecoder().poll();
    if (videoDecoder().error()[0] != '\0' && !videoDecoder().hasFrame()) {
        ImGui::TextColored(theme::danger(), "%s", videoDecoder().error());
        return;
    }
    if (!videoDecoder().hasFrame() || videoDecoder().width() <= 0 || videoDecoder().height() <= 0) {
        drawVideoBuffering(avail);
        return;
    }
    blitDecodedVideo(avail);
}

void blitDecodedVideo(ImVec2 avail) {
    const float vw = static_cast<float>(videoDecoder().width());
    const float vh = static_cast<float>(videoDecoder().height());
    if (vw <= 0.0f || vh <= 0.0f || avail.x <= 1.0f || avail.y <= 1.0f) {
        return;
    }
    // 90° CCW + vertical flip: displayed aspect is swapped.
    const float dw = vh;
    const float dh = vw;
    const float scale = std::min(avail.x / dw, avail.y / dh);
    const ImVec2 sz(dw * scale, dh * scale);
    const ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - sz.x) * 0.5f, cursor.y + (avail.y - sz.y) * 0.5f));
    ImGui::Dummy(sz);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    const ImTextureRef tex(static_cast<ImTextureID>(static_cast<std::uintptr_t>(videoDecoder().texture())));
    // 90° CCW then flip top/bottom so the camera is right-side up.
    ImGui::GetWindowDrawList()->AddImageQuad(tex, ImVec2(a.x, a.y), ImVec2(b.x, a.y), ImVec2(b.x, b.y),
                                             ImVec2(a.x, b.y), ImVec2(0.0f, 0.0f), ImVec2(0.0f, 1.0f),
                                             ImVec2(1.0f, 1.0f), ImVec2(1.0f, 0.0f));
}

static void drawVideoAndHud(ClientArena& arena, ImVec2 avail) {
    const float gap = 12.0f;
    const float hudW = std::clamp(std::min(avail.y * 0.48f, 252.0f), 200.0f, 268.0f);
    const bool side = avail.x > hudW + 120.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float videoW = side ? avail.x - hudW - gap : avail.x;
    drawVideoImage(ImVec2(videoW, avail.y));
    if (side) {
        const ImVec2 a(origin.x + videoW + gap, origin.y);
        const ImVec2 b(origin.x + avail.x, origin.y + avail.y);
        drawVideoHud(arena, a, b);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(avail);
}

// One line describing where video is relative to the cursor. Without it the
// three interesting failures — cursor outside the buffered window, unverified
// A/V clock, no pointer available — all look identical to "no video".
static void drawCursorPlaybackStatus(ClientArena& arena) {
    const VideoDecoder& dec = videoDecoder();
    const char* modeName = "idle";
    switch (dec.mode()) {
        case vidx::PlaybackMode::Live:
            modeName = "cache";
            break;
        case vidx::PlaybackMode::Scrubbing:
            modeName = "scrub";
            break;
        case vidx::PlaybackMode::Committed:
            modeName = "cursor";
            break;
        case vidx::PlaybackMode::Idle:
            break;
    }
    const vidx::CursorPlayback& pb = dec.playback();
    char line[160];
    std::snprintf(line, sizeof(line), "%s  %zu kB", modeName, dec.bufferedBytes() / 1024);
    if (dec.mode() != vidx::PlaybackMode::Live && dec.mode() != vidx::PlaybackMode::Idle) {
        std::uint64_t locateAt = 0;
        const std::uint64_t cursorMs = arena.displayCursorMs();
        if (dec.pendingLocate(locateAt)) {
            std::snprintf(line, sizeof(line), "%s  locating…", modeName);
        } else if (!pb.covers(cursorMs) && !pb.coverage().empty() && !pb.covered(cursorMs)) {
            std::snprintf(line, sizeof(line), "%s  no video at cursor", modeName);
        }
    }
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + std::max(32.0f, w));
    ImGui::TextColored(theme::mutedFg(), "%s", line);
    ImGui::PopTextWrapPos();
}

void drawVideoImage(ClientArena& arena, ImVec2 avail) {
    if (!videoDecoder().supported()) {
        ImGui::TextColored(theme::warning(), "Video decoder unavailable");
        return;
    }
    // Video follows the cursor, so the plan is recomputed once per UI frame
    // before anything is drawn.
    videoDecoder().syncToCursor(arena);
    drawCursorPlaybackStatus(arena);
    const ImVec2 left = ImGui::GetContentRegionAvail();
    const ImVec2 blit(std::min(avail.x, left.x), std::min(avail.y, left.y));
    drawVideoAndHud(arena, blit);
}

void drawVideoTile(ClientArena& arena, ImVec2 size) {
    theme::beginCard("videotile", size, theme::card());
    ImGui::TextColored(theme::primary(), "Live View");
    ImGui::SameLine();
    if (!videoDecoder().supported()) {
        ImGui::TextColored(theme::mutedFg(), "unsupported");
        ImGui::TextColored(theme::warning(), "Video decoder unavailable");
        theme::endCard();
        return;
    }
    videoDecoder().syncToCursor(arena);
    const auto& v = arena.video;
    std::uint64_t locateAt = 0;
    if (videoDecoder().hasFrame()) {
        ImGui::TextColored(theme::success(), "cache  %s  %0.0f fps  %zu kB", v.mode.c_str(), v.fps,
                           videoDecoder().bufferedBytes() / 1024);
    } else if (!v.lastError.empty()) {
        ImGui::TextColored(theme::warning(), "video store  %s", v.lastError.c_str());
    } else if (videoDecoder().pendingLocate(locateAt)) {
        ImGui::TextColored(theme::warning(), "buffering…");
    } else if (v.haveStatus) {
        ImGui::TextColored(v.connected ? theme::success() : theme::warning(), "%s  %s  %0.0f fps  drop %llu",
                           v.connected ? "up" : "down", v.mode.c_str(), v.fps,
                           static_cast<unsigned long long>(v.dropped));
    } else if (v.subscribed && !v.lastError.empty()) {
        ImGui::TextColored(theme::warning(), "store down  %s", v.lastError.c_str());
    } else if (v.subscribed) {
        ImGui::TextColored(theme::mutedFg(), "pointer cache: waiting for GOP");
    } else {
        ImGui::TextColored(theme::mutedFg(), "idle");
    }
    drawCursorPlaybackStatus(arena);
    drawVideoAndHud(arena, ImGui::GetContentRegionAvail());
    theme::endCard();
}

}  // namespace photon::ui
