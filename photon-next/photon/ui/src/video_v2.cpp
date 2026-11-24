#include "photon_ui/video_v2.hpp"

#include "photon/protocol.hpp"
#include "photon_ui/clock_format.hpp"

#include <cmath>
#include <cstdio>
#include <utility>

namespace photon::ui {
namespace {

// First and last bits of a VideoChunk, cleared from the flags a frame keeps.
constexpr std::uint8_t kFirstAndLast = proto2::kVideoFirst | proto2::kVideoLast;

std::string clockOf(std::uint64_t ms) {
    char buf[48];
    formatLocalClock(ms, buf, sizeof(buf));
    return buf;
}

// Starts a request in the session: what it asks for, and that its status is awaited.
VideoCommand sendRequest(VideoSession& s, VideoOp op, std::uint64_t cursorMs, double nowMs,
                         std::uint32_t qid) {
    switch (op) {
        case VideoOp::SubscribeLive:
            s.stream = VideoStream::Live;
            break;
        case VideoOp::SubscribeReplay:
        case VideoOp::Seek:
            s.stream = VideoStream::Replay;
            break;
        case VideoOp::Off:
            s.stream = VideoStream::Off;
            break;
        case VideoOp::None:
            return VideoCommand{};
    }
    if (op == VideoOp::SubscribeReplay || op == VideoOp::Seek) {
        s.sentCursorMs = cursorMs;
        s.sentAtMs = nowMs;
        s.lastSeekMs = nowMs;
        s.stillUntilMs = nowMs + kVideoStillMs;
    }
    if (op == VideoOp::Seek) {
        s.seekQid = qid;
    }
    s.awaiting = true;
    s.awaitingSinceMs = nowMs;
    s.awaitingStream = s.stream;
    s.awaitingQid = op == VideoOp::Seek ? qid : 0;
    return VideoCommand{op, cursorMs, qid};
}

}  // namespace

std::optional<std::size_t> VideoAssembler::indexOf(std::uint8_t stream) const {
    for (std::size_t i = 0; i < partials_.size(); ++i) {
        if (partials_[i].stream == stream) {
            return i;
        }
    }
    return std::nullopt;
}

void VideoAssembler::abandon(std::size_t index) {
    partials_.erase(partials_.begin() + static_cast<std::ptrdiff_t>(index));
    ++dropped_;
}

std::optional<AssembledFrame> VideoAssembler::push(const proto2::VideoChunk& chunk) {
    const bool first = (chunk.flags & proto2::kVideoFirst) != 0;
    const bool last = (chunk.flags & proto2::kVideoLast) != 0;
    const std::optional<std::size_t> held = indexOf(chunk.stream);

    if (first) {
        if (held) {
            abandon(*held);  // the frame before it never got its last piece
        }
    } else if (!held || partials_[*held].frame != chunk.frame) {
        // The tail of a frame whose head is not here: that frame is dropped.
        if (held) {
            abandon(*held);
        }
        return std::nullopt;
    }

    if (!last && chunk.payload.size() != proto2::kVideoChunkMax) {
        // Only the last piece may be short, so bytes went missing from this frame.
        if (!first) {
            abandon(*held);
        }
        return std::nullopt;
    }

    if (first) {
        const auto flags = static_cast<std::uint8_t>(chunk.flags & ~kFirstAndLast);
        if (last) {
            return AssembledFrame{chunk.tMs, chunk.frame, flags, chunk.stream, chunk.payload};
        }
        partials_.push_back(Partial{chunk.frame, chunk.tMs, flags, chunk.stream, chunk.payload});
        return std::nullopt;
    }

    Partial& p = partials_[*held];
    p.payload.insert(p.payload.end(), chunk.payload.begin(), chunk.payload.end());
    if (!last) {
        return std::nullopt;
    }
    AssembledFrame out{p.tMs, p.frame, p.flags, p.stream, std::move(p.payload)};
    partials_.erase(partials_.begin() + static_cast<std::ptrdiff_t>(*held));
    return out;
}

void VideoAssembler::reset() {
    partials_.clear();
}

std::uint8_t decoderFlags(std::uint8_t chunkFlags) {
    std::uint8_t out = 0;
    if ((chunkFlags & proto2::kVideoKeyframe) != 0) {
        out |= proto::kVideoFlagKeyframe;
    }
    if ((chunkFlags & proto2::kVideoConfig) != 0) {
        out |= proto::kVideoFlagConfig;
    }
    return out;
}

std::uint32_t nextVideoQid(std::uint32_t qid) {
    if (qid < kVideoQidBase || qid >= kVideoQidEnd - 1) {
        return kVideoQidBase;
    }
    return qid + 1;
}

std::string videoGapText(const ctrl2::VideoGap& gap) {
    const auto from = static_cast<std::uint64_t>(gap.fromMs < 0 ? 0 : gap.fromMs);
    const auto to = static_cast<std::uint64_t>(gap.toMs < 0 ? 0 : gap.toMs);
    if (gap.reason == "archived") {
        return "Video for this time is archived, not on the server";
    }
    if (gap.reason == "stale") {
        return "Camera offline since " + clockOf(from);
    }
    if (gap.reason == "none") {
        if (to == 0) {
            return "No video recorded since " + clockOf(from);
        }
        return "No video recorded " + clockOf(from) + "\xE2\x80\x93" + clockOf(to);  // en dash
    }
    return "Video unavailable";
}

const char* videoStreamName(VideoStream stream) {
    switch (stream) {
        case VideoStream::Off:
            return "off";
        case VideoStream::Live:
            return "live";
        case VideoStream::Replay:
            return "replay";
    }
    return "off";
}

VideoCommand decideVideo(const VideoInputs& in, VideoSession& s, double nowMs) {
    if (!in.connected || !in.serverHasVideo) {
        return VideoCommand{};
    }
    if (s.awaiting) {
        if (nowMs - s.awaitingSinceMs < kVideoAwaitMs) {
            return VideoCommand{};
        }
        s.awaiting = false;  // the answer never came: the stream is unknown, so it is asked for again
        s.stream = VideoStream::Off;
    }
    if (nowMs < s.blockedUntilMs) {
        return VideoCommand{};
    }

    const bool wanted = nowMs - in.lastWantedMs < kVideoIdleOffMs;
    if (!wanted) {
        return s.stream != VideoStream::Off ? sendRequest(s, VideoOp::Off, 0, nowMs, 0) : VideoCommand{};
    }
    if (!in.pinned) {
        return s.stream != VideoStream::Live ? sendRequest(s, VideoOp::SubscribeLive, 0, nowMs, 0)
                                             : VideoCommand{};
    }

    const bool rateOk = nowMs - s.lastSeekMs >= kVideoSeekGapMs;
    if (in.playing) {
        if (s.stream != VideoStream::Replay) {
            return sendRequest(s, VideoOp::SubscribeReplay, in.cursorMs, nowMs, 0);
        }
        // The server plays at its own wall clock from the time it was last told: a
        // cursor away from that time is a jump.
        const double expected = static_cast<double>(s.sentCursorMs) + (nowMs - s.sentAtMs);
        const bool jumped = std::fabs(static_cast<double>(in.cursorMs) - expected) > kVideoJumpMs;
        if (jumped && rateOk) {
            return sendRequest(s, VideoOp::Seek, in.cursorMs, nowMs, nextVideoQid(s.seekQid));
        }
        return VideoCommand{};
    }

    // Paused or dragged: the picture is the frame at the cursor. A replay stream
    // starts or moves to a new cursor, and stops once the frame has come.
    if (in.cursorMs != s.sentCursorMs || s.stream == VideoStream::Live) {
        if (!rateOk) {
            return VideoCommand{};
        }
        return s.stream == VideoStream::Replay
                   ? sendRequest(s, VideoOp::Seek, in.cursorMs, nowMs, nextVideoQid(s.seekQid))
                   : sendRequest(s, VideoOp::SubscribeReplay, in.cursorMs, nowMs, 0);
    }
    if (s.stream == VideoStream::Replay && nowMs >= s.stillUntilMs) {
        return sendRequest(s, VideoOp::Off, 0, nowMs, 0);
    }
    return VideoCommand{};
}

bool noteVideoStatus(VideoSession& s, const ctrl2::VideoStatus& status) {
    if (!s.awaiting || status.mode != videoStreamName(s.awaitingStream)) {
        return false;
    }
    if (s.awaitingQid != 0 && (!status.qid || *status.qid != s.awaitingQid)) {
        return false;
    }
    s.awaiting = false;
    return true;
}

void noteVideoError(VideoSession& s, double nowMs) {
    s.awaiting = false;
    s.stream = VideoStream::Off;
    s.blockedUntilMs = nowMs + kVideoRetryMs;
}

}  // namespace photon::ui
