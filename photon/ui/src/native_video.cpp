#ifndef __EMSCRIPTEN__

#include "photon_ui/gl_api.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <oleauto.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kMaxPendingRgba = 1;

struct NativeVideo {
    std::mutex mu;
    bool started = false;
    bool ok = false;
    bool configured = false;
    std::string error;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
    std::vector<std::uint8_t> rgba;
    bool haveRgba = false;
    double lastKeyTs = -1.0;
    int lastKeyLen = 0;
    ComPtr<IMFTransform> dec;
};

NativeVideo gVid;

int startCodeLen(const std::uint8_t* p, int n) {
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        return 4;
    }
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        return 3;
    }
    return 0;
}

int nalType(const std::uint8_t* p, int n) {
    const int i = startCodeLen(p, n);
    return i < n ? (p[i] & 0x1f) : 0;
}

int findStartCode(const std::uint8_t* p, int n, int from) {
    for (int i = from; i < n; ++i) {
        if (startCodeLen(p + i, n - i) != 0) {
            return i;
        }
    }
    return n;
}

template <typename Fn>
void forEachAnnexB(const std::uint8_t* p, int n, Fn&& fn) {
    if (p == nullptr || n <= 0) {
        return;
    }
    if (startCodeLen(p, n) == 0) {
        fn(p, n);
        return;
    }
    int pos = 0;
    while (pos < n) {
        const int sc = startCodeLen(p + pos, n - pos);
        if (sc == 0) {
            break;
        }
        const int next = findStartCode(p, n, pos + sc);
        fn(p + pos, next - pos);
        pos = next;
    }
}

bool sameBytes(const std::vector<std::uint8_t>& a, const std::uint8_t* p, int n) {
    return static_cast<int>(a.size()) == n && (n == 0 || std::memcmp(a.data(), p, static_cast<std::size_t>(n)) == 0);
}

void nv12ToRgba(const std::uint8_t* y, int yStride, const std::uint8_t* uv, int uvStride, int w, int h,
                std::uint8_t* rgba) {
    for (int row = 0; row < h; ++row) {
        const std::uint8_t* yRow = y + row * yStride;
        const std::uint8_t* uvRow = uv + (row / 2) * uvStride;
        std::uint8_t* out = rgba + row * w * 4;
        for (int col = 0; col < w; ++col) {
            const int Y = yRow[col];
            const int U = uvRow[(col & ~1)] - 128;
            const int V = uvRow[(col & ~1) + 1] - 128;
            const int c = Y - 16;
            int r = (298 * c + 409 * V + 128) >> 8;
            int g = (298 * c - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * c + 516 * U + 128) >> 8;
            out[col * 4 + 0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
            out[col * 4 + 1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
            out[col * 4 + 2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
            out[col * 4 + 3] = 255;
        }
    }
}

int gLogLeft = 32;

void vlog(const char* fmt, ...) {
    if (gLogLeft <= 0 || fmt == nullptr) {
        return;
    }
    --gLogLeft;
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[video] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    va_end(ap);
}

void setError(const char* msg) {
    gVid.error = msg != nullptr ? msg : "";
    if (msg != nullptr && msg[0] != '\0') {
        vlog("error %s", msg);
    }
}

void closeDecoder() {
    if (gVid.dec != nullptr) {
        gVid.dec->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        gVid.dec->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        gVid.dec.Reset();
    }
    gVid.configured = false;
}

struct BitReader {
    const std::uint8_t* p = nullptr;
    int n = 0;
    int bit = 0;
    bool ok = true;

    explicit BitReader(const std::vector<std::uint8_t>& rbsp) : p(rbsp.data()), n(static_cast<int>(rbsp.size())) {}

    std::uint32_t u(int bits) {
        std::uint32_t v = 0;
        for (int i = 0; i < bits; ++i) {
            const int byte = bit / 8;
            if (byte >= n) {
                ok = false;
                return 0;
            }
            v = (v << 1) | static_cast<std::uint32_t>((p[byte] >> (7 - (bit % 8))) & 1);
            ++bit;
        }
        return v;
    }
    std::uint32_t ue() {
        int z = 0;
        while (ok && u(1) == 0) {
            ++z;
            if (z > 31) {
                ok = false;
                return 0;
            }
        }
        if (!ok) {
            return 0;
        }
        return ((1u << z) - 1u) + u(z);
    }
};

std::vector<std::uint8_t> rbspFromNal(const std::uint8_t* nal, int nalLen) {
    std::vector<std::uint8_t> out;
    const int sc = startCodeLen(nal, nalLen);
    int i = sc + 1;
    out.reserve(static_cast<std::size_t>(std::max(0, nalLen - i)));
    while (i < nalLen) {
        if (i + 2 < nalLen && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
            out.push_back(0);
            out.push_back(0);
            i += 3;
            continue;
        }
        out.push_back(nal[i]);
        ++i;
    }
    return out;
}

bool parseSpsSize(const std::uint8_t* nal, int nalLen, int& w, int& h) {
    const auto rbsp = rbspFromNal(nal, nalLen);
    if (rbsp.size() < 4) {
        return false;
    }
    BitReader br(rbsp);
    const std::uint32_t profile = br.u(8);
    br.u(8);
    br.u(8);
    br.ue();
    const bool high = profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
                      profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138;
    if (high) {
        const std::uint32_t chroma = br.ue();
        if (chroma == 3) {
            br.u(1);
        }
        br.ue();
        br.ue();
        br.u(1);
        if (br.u(1)) {
            return false;
        }
    }
    br.ue();
    const std::uint32_t poc = br.ue();
    if (poc == 0) {
        br.ue();
    } else if (poc == 1) {
        br.u(1);
        br.ue();
        br.ue();
        const std::uint32_t n = br.ue();
        for (std::uint32_t i = 0; i < n; ++i) {
            br.ue();
        }
    }
    br.ue();
    br.u(1);
    const std::uint32_t wMbs = br.ue();
    const std::uint32_t hMap = br.ue();
    const std::uint32_t frameOnly = br.u(1);
    if (!br.ok) {
        return false;
    }
    w = static_cast<int>((wMbs + 1) * 16);
    h = static_cast<int>((hMap + 1) * 16 * (2 - frameOnly));
    return w > 0 && h > 0 && w <= 4096 && h <= 4096;
}

bool setOutputNv12(IMFTransform* dec) {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> outType;
        if (FAILED(dec->GetOutputAvailableType(0, i, &outType))) {
            break;
        }
        GUID sub{};
        if (FAILED(outType->GetGUID(MF_MT_SUBTYPE, &sub))) {
            continue;
        }
        if (sub == MFVideoFormat_NV12) {
            return SUCCEEDED(dec->SetOutputType(0, outType.Get(), 0));
        }
    }
    return false;
}

bool setIoTypes(IMFTransform* dec) {
    ComPtr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(&inType))) {
        return false;
    }
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (!gVid.sps.empty()) {
        std::vector<std::uint8_t> blob = gVid.sps;
        blob.insert(blob.end(), gVid.pps.begin(), gVid.pps.end());
        inType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), static_cast<UINT32>(blob.size()));
        int sw = 0;
        int sh = 0;
        if (parseSpsSize(gVid.sps.data(), static_cast<int>(gVid.sps.size()), sw, sh)) {
            MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(sw), static_cast<UINT32>(sh));
            if (gVid.width == 0) {
                gVid.width = sw;
                gVid.height = sh;
            }
            vlog("sps %dx%d", sw, sh);
        }
    }
    if (FAILED(dec->SetInputType(0, inType.Get(), 0))) {
        return false;
    }
    return setOutputNv12(dec);
}

bool ensureDecoder() {
    if (gVid.dec != nullptr) {
        return true;
    }
    ComPtr<IMFTransform> dec;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dec)))) {
        setError("H.264 decoder unavailable");
        return false;
    }
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(dec.As(&codec)) && codec != nullptr) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        VariantClear(&v);
    }
    if (!setIoTypes(dec.Get())) {
        setError("H.264 decoder rejected NV12");
        return false;
    }
    dec->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    dec->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    gVid.dec = dec;
    gVid.configured = true;
    gVid.error.clear();
    return true;
}

bool copyOutput(IMFSample* sample) {
    if (sample == nullptr) {
        return false;
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || buffer == nullptr) {
        return false;
    }
    ComPtr<IMF2DBuffer> buf2d;
    BYTE* scan = nullptr;
    LONG stride = 0;
    BYTE* raw = nullptr;
    DWORD rawMax = 0;
    DWORD rawLen = 0;
    UINT32 w = static_cast<UINT32>(gVid.width);
    UINT32 h = static_cast<UINT32>(gVid.height);
    ComPtr<IMFMediaType> type;
    if (gVid.dec != nullptr && SUCCEEDED(gVid.dec->GetOutputCurrentType(0, &type)) && type != nullptr) {
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
    }
    if ((w == 0 || h == 0) && !gVid.sps.empty()) {
        int sw = 0;
        int sh = 0;
        if (parseSpsSize(gVid.sps.data(), static_cast<int>(gVid.sps.size()), sw, sh)) {
            w = static_cast<UINT32>(sw);
            h = static_cast<UINT32>(sh);
        }
    }
    if (w == 0 || h == 0) {
        vlog("copyOutput missing frame size");
        return false;
    }
    const std::uint8_t* y = nullptr;
    const std::uint8_t* uv = nullptr;
    int yStride = static_cast<int>(w);
    int uvStride = static_cast<int>(w);
    if (SUCCEEDED(buffer.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&scan, &stride)) && scan != nullptr) {
        y = scan;
        yStride = stride;
        uv = scan + stride * static_cast<LONG>(h);
        uvStride = stride;
        gVid.rgba.resize(static_cast<std::size_t>(w) * h * 4);
        nv12ToRgba(y, yStride, uv, uvStride, static_cast<int>(w), static_cast<int>(h), gVid.rgba.data());
        buf2d->Unlock2D();
    } else if (SUCCEEDED(buffer->Lock(&raw, &rawMax, &rawLen)) && raw != nullptr) {
        y = raw;
        uv = raw + static_cast<std::size_t>(w) * h;
        gVid.rgba.resize(static_cast<std::size_t>(w) * h * 4);
        nv12ToRgba(y, yStride, uv, uvStride, static_cast<int>(w), static_cast<int>(h), gVid.rgba.data());
        buffer->Unlock();
    } else {
        return false;
    }
    gVid.width = static_cast<int>(w);
    gVid.height = static_cast<int>(h);
    gVid.haveRgba = true;
    (void)kMaxPendingRgba;
    return true;
}

void drainOutputs() {
    if (gVid.dec == nullptr) {
        return;
    }
    MFT_OUTPUT_STREAM_INFO info{};
    gVid.dec->GetOutputStreamInfo(0, &info);
    for (int i = 0; i < 8; ++i) {
        MFT_OUTPUT_DATA_BUFFER out{};
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> mem;
        if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            if (FAILED(MFCreateSample(&sample))) {
                break;
            }
            const DWORD bytes = info.cbSize != 0 ? info.cbSize : 1;
            if (FAILED(MFCreateAlignedMemoryBuffer(bytes, info.cbAlignment > 1 ? info.cbAlignment : 16, &mem))) {
                break;
            }
            sample->AddBuffer(mem.Get());
            out.pSample = sample.Get();
        }
        DWORD status = 0;
        const HRESULT hr = gVid.dec->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents != nullptr) {
            out.pEvents->Release();
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            vlog("stream change — renegotiate NV12 only");
            if (!setOutputNv12(gVid.dec.Get())) {
                setError("H.264 decoder lost NV12 after stream change");
                break;
            }
            continue;
        }
        if (FAILED(hr) || out.pSample == nullptr) {
            if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
                vlog("ProcessOutput 0x%08lX", static_cast<unsigned long>(hr));
            }
            break;
        }
        copyOutput(out.pSample);
        vlog("picture %dx%d rgba=%d", gVid.width, gVid.height, gVid.haveRgba ? 1 : 0);
        if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0) {
            out.pSample->Release();
        }
    }
}

bool submitAu(const std::uint8_t* data, int len, double tsMs) {
    if (!ensureDecoder() || data == nullptr || len <= 0) {
        return false;
    }
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(len), &buf))) {
        return false;
    }
    BYTE* dst = nullptr;
    if (FAILED(buf->Lock(&dst, nullptr, nullptr)) || dst == nullptr) {
        return false;
    }
    std::memcpy(dst, data, static_cast<std::size_t>(len));
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(len));
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) {
        return false;
    }
    sample->AddBuffer(buf.Get());
    // Epoch videoMs overflows some MF clocks. A monotonic 30 fps stamp is enough
    // for a pointer-cache still.
    static LONGLONG sStamp = 0;
    sStamp += 333667;
    sample->SetSampleTime(sStamp);
    sample->SetSampleDuration(333667);
    (void)tsMs;
    HRESULT hr = gVid.dec->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        drainOutputs();
        hr = gVid.dec->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        char msg[80];
        std::snprintf(msg, sizeof(msg), "H.264 ProcessInput failed 0x%08lX", static_cast<unsigned long>(hr));
        setError(msg);
        closeDecoder();
        return false;
    }
    drainOutputs();
    return true;
}

void drainForStill() {
    if (gVid.dec == nullptr || gVid.haveRgba) {
        return;
    }
    vlog("drain for still");
    gVid.dec->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    drainOutputs();
    gVid.dec->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

}  // namespace

extern "C" {

int photon_video_supported() {
    std::lock_guard<std::mutex> lock(gVid.mu);
    return !gVid.started || gVid.ok ? 1 : 0;
}

void photon_video_init() {
    std::lock_guard<std::mutex> lock(gVid.mu);
    if (gVid.started) {
        return;
    }
    gVid.started = true;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        setError("Media Foundation failed to start");
        gVid.ok = false;
        return;
    }
    gVid.ok = true;
    gVid.error.clear();
}

void photon_video_reset() {
    std::lock_guard<std::mutex> lock(gVid.mu);
    closeDecoder();
    gVid.sps.clear();
    gVid.pps.clear();
    gVid.rgba.clear();
    gVid.haveRgba = false;
    gVid.width = 0;
    gVid.height = 0;
    gVid.lastKeyTs = -1.0;
    gVid.lastKeyLen = 0;
    if (gVid.ok) {
        gVid.error.clear();
    }
}

void photon_video_copy_error(char* ptr, int cap) {
    if (ptr == nullptr || cap <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(gVid.mu);
    std::snprintf(ptr, static_cast<std::size_t>(cap), "%s", gVid.error.c_str());
}

int photon_video_dim(int which) {
    std::lock_guard<std::mutex> lock(gVid.mu);
    return which == 0 ? gVid.width : gVid.height;
}

void photon_video_end_au(int force) {
    std::lock_guard<std::mutex> lock(gVid.mu);
    if (!force) {
        return;
    }
    drainForStill();
}

void photon_video_feed(const std::uint8_t* ptr, int len, double tsMs, int flags) {
    if (ptr == nullptr || len <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(gVid.mu);
    if (!gVid.ok) {
        return;
    }
    // Car cameras and locate/replay windows often pack AUD+SPS+PPS+IDR in one
    // buffer. The first NAL type is then 9 (AUD), which the old filter dropped
    // wholesale, or 7 (SPS), which stored the whole AU as "SPS" and never
    // decoded. Walk every start code.
    forEachAnnexB(ptr, len, [&](const std::uint8_t* nal, int nalLen) {
        const int t = nalType(nal, nalLen);
        if (t == 7) {
            const bool changed = !sameBytes(gVid.sps, nal, nalLen);
            gVid.sps.assign(nal, nal + nalLen);
            if (changed) {
                closeDecoder();
            }
            return;
        }
        if (t == 8) {
            gVid.pps.assign(nal, nal + nalLen);
            return;
        }
        if (t != 1 && t != 5) {
            return;
        }
        if (t == 5 && gVid.haveRgba && gVid.lastKeyTs == tsMs && gVid.lastKeyLen == nalLen) {
            return;
        }
        if (t == 5) {
            gVid.lastKeyTs = tsMs;
            gVid.lastKeyLen = nalLen;
        }
        if (t == 5 && gVid.sps.empty()) {
            vlog("IDR before SPS — waiting");
            return;
        }
        vlog("nal t=%d len=%d flags=%d sps=%zu pps=%zu", t, nalLen, flags, gVid.sps.size(),
             gVid.pps.size());
        if (t == 5 && !gVid.sps.empty() && !gVid.pps.empty()) {
            std::vector<std::uint8_t> au;
            au.reserve(gVid.sps.size() + gVid.pps.size() + static_cast<std::size_t>(nalLen));
            au.insert(au.end(), gVid.sps.begin(), gVid.sps.end());
            au.insert(au.end(), gVid.pps.begin(), gVid.pps.end());
            au.insert(au.end(), nal, nal + nalLen);
            submitAu(au.data(), static_cast<int>(au.size()), tsMs);
            drainForStill();
            return;
        }
        submitAu(nal, nalLen, tsMs);
        if (t == 5) {
            drainForStill();
        }
        (void)flags;
    });
}

int photon_video_upload(unsigned glTex) {
    std::lock_guard<std::mutex> lock(gVid.mu);
    if (!gVid.haveRgba || gVid.width <= 0 || gVid.height <= 0 || glTex == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, glTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gVid.width, gVid.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 gVid.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return 1;
}

}  // extern "C"

#endif
