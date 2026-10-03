#include "photon/session.hpp"

#include <cstdio>

namespace photon {
namespace {

constexpr std::size_t kVideoCongestBytes = 512 * 1024;

}  // namespace

void SessionHub::streamVideo() {
    auto frames = videoRelay_.takeFrames();
    if (frames.empty()) {
        return;
    }
    static std::uint64_t wsVideo = 0;
    wsVideo += frames.size();
    if (wsVideo <= 3 || wsVideo % 200 == 0) {
        std::fprintf(stdout, "[photon-backend] video ws nals=%llu pending=%zu clients=%zu\n",
                     static_cast<unsigned long long>(wsVideo), frames.size(), clients_.size());
        std::fflush(stdout);
    }
    for (const auto& frame : frames) {
        proto::VideoFrame vf;
        vf.timestampMs = frame.timestampMs;
        vf.flags = frame.flags;
        vf.nal = frame.payload;
        const auto payload = proto::encodeVideoFrame(vf);
        const bool key = proto::videoKeyframe(frame.flags);
        const bool cfg = proto::videoConfig(frame.flags);
        for (auto& client : clients_) {
            if (!client->helloOk || !client->videoSubscribed) {
                continue;
            }
            // History tiles share this socket. Counting them as video congestion
            // dropped every P-frame, so the picture only changed on the next
            // keyframe (about once a GOP). Only a backed-up video/telemetry
            // queue should skip deltas.
            std::size_t history = 0;
            for (const auto& tag : client->outboundTags) {
                if (tag.history) {
                    history += tag.bytes;
                }
            }
            const std::size_t out = client->conn.outboundBytes();
            if (history > out) {
                history = out;
            }
            if (out - history > kVideoCongestBytes && !key && !cfg) {
                videoRelay_.noteDropped();
                continue;
            }
            sendBinary(*client, proto::MsgType::VideoFrame, payload);
        }
    }
}

void SessionHub::streamVideoStatus(std::uint64_t nowTick, bool force) {
    const bool changed = videoRelay_.consumeStatusChanged();
    if (!force && !changed) {
        if (!videoRelay_.hasSubscribers()) {
            return;
        }
        if (lastVideoStatusMs_ != 0 && nowTick - lastVideoStatusMs_ < 1000) {
            return;
        }
    }
    lastVideoStatusMs_ = nowTick;
    const auto msg = videoRelay_.statusMessage();
    for (auto& client : clients_) {
        if (client->helloOk && client->videoSubscribed) {
            sendControl(*client, msg);
        }
    }
}

}  // namespace photon
