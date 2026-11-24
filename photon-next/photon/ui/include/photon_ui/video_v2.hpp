#pragma once

#include "photon/control_v2.hpp"
#include "photon/protocol_v2.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Protocol v2 video, as pure logic: reassembling VideoChunk pieces into frames, the
// qids of video_seek, the subscription policy (live, replay, seek, off) and the text
// a gap shows. No I/O and no ImGui, so the native tests cover it. The wire is in
// photon-next/docs/protocol-v2.md.
namespace photon::ui {

// --- Frames ------------------------------------------------------------------

// One whole Annex-B frame: its pieces joined. flags keeps the keyframe, config and
// replay bits of the chunk; first and last are cleared.
struct AssembledFrame {
    std::uint64_t tMs = 0;
    std::uint32_t frame = 0;
    std::uint8_t flags = 0;
    std::uint8_t stream = 0;
    std::vector<std::uint8_t> payload;
};

// Rebuilds frames from VideoChunk pieces, one partial frame per stream. A piece with
// the first bit starts a frame and ends a partial one that never got its last piece.
// A frame whose piece is missing is dropped; the decoder waits for the next keyframe.
// Every piece but the last of a frame is exactly kVideoChunkMax bytes (the server cuts
// it so), so a short one means bytes went missing and the frame is dropped too. The
// wire has no piece index, so a middle piece that vanishes while the last arrives
// cannot be seen from the bytes; the server never sends that (it drops its whole
// queue), so it does not happen.
class VideoAssembler {
public:
    std::optional<AssembledFrame> push(const proto2::VideoChunk& chunk);
    // Forgets every partial frame: a new stream starts.
    void reset();
    // Partial frames given up (a piece missing, or a new frame before the last piece).
    std::uint32_t dropped() const { return dropped_; }

private:
    struct Partial {
        std::uint32_t frame = 0;
        std::uint64_t tMs = 0;
        std::uint8_t flags = 0;
        std::uint8_t stream = 0;
        std::vector<std::uint8_t> payload;
    };
    std::vector<Partial> partials_;
    std::uint32_t dropped_ = 0;

    std::optional<std::size_t> indexOf(std::uint8_t stream) const;
    void abandon(std::size_t index);
};

// The decoder's flags (protocol.hpp kVideoFlagKeyframe and kVideoFlagConfig) for a
// VideoChunk's flags. The two bit layouts agree on bits 0 and 1; first, last and
// replay are not decoder flags.
std::uint8_t decoderFlags(std::uint8_t chunkFlags);
inline bool isReplayChunk(std::uint8_t chunkFlags) { return (chunkFlags & proto2::kVideoReplay) != 0; }

// What the client hands the decoder, in order: a restart (a new stream began, so
// the decoder forgets what it held) or one frame.
struct VideoEvent {
    enum class Kind : std::uint8_t { Restart, Frame };
    Kind kind = Kind::Frame;
    std::uint64_t tMs = 0;
    std::uint8_t flags = 0;  // decoder flags
    std::vector<std::uint8_t> payload;
};

// --- Seek qids ---------------------------------------------------------------

// video_seek qids live in [kVideoQidBase, kVideoQidEnd). Tile qids are below and
// analysis qids (kAnalysisQueryOrigin) from 0x80000000 up, so a video qid never
// reaches the tile path.
inline constexpr std::uint32_t kVideoQidBase = 0x40000000u;
inline constexpr std::uint32_t kVideoQidEnd = 0x80000000u;

// The qid after qid, wrapping inside the range. 0 (or any qid outside it) gives the first.
std::uint32_t nextVideoQid(std::uint32_t qid);
inline bool isVideoQid(std::uint32_t qid) { return qid >= kVideoQidBase && qid < kVideoQidEnd; }

// --- Gap text ----------------------------------------------------------------

// The line a gap shows over the video panels, with local clock times.
std::string videoGapText(const ctrl2::VideoGap& gap);

// --- Subscription policy -----------------------------------------------------

// The subscription is off unless a screen wants video (kVideoIdleOffMs after the
// last time one did).
inline constexpr double kVideoIdleOffMs = 5000.0;
// Scrubs send at most one seek this often.
inline constexpr double kVideoSeekGapMs = 250.0;
// Play runs the server's replay clock; a cursor this far from where the server is
// is a jump, and is sent as a seek.
inline constexpr double kVideoJumpMs = 1000.0;
// A paused cursor keeps its replay stream this long (the frames up to the cursor
// arrive at once, the server runs ahead by its lead), then the stream is turned off.
inline constexpr double kVideoStillMs = 1500.0;
// A request whose video_status has not come in this long is asked again.
inline constexpr double kVideoAwaitMs = 3000.0;
// After a server error for a request, nothing is sent for this long.
inline constexpr double kVideoRetryMs = 1000.0;

enum class VideoStream : std::uint8_t { Off, Live, Replay };
const char* videoStreamName(VideoStream stream);  // "off", "live" or "replay"

// What the client has asked the server for, and what it still waits on.
struct VideoSession {
    VideoStream stream = VideoStream::Off;
    std::uint64_t sentCursorMs = 0;  // the cursor of the last replay request or seek
    double sentAtMs = 0.0;           // when that was sent
    double lastSeekMs = -1.0e12;     // when the last replay request or seek was sent
    double stillUntilMs = 0.0;       // a paused replay stream is turned off at this time
    double blockedUntilMs = 0.0;     // no request before this (after a server error)
    std::uint32_t seekQid = 0;       // the qid of the last video_seek
    bool awaiting = false;           // a request was sent and its video_status has not come
    double awaitingSinceMs = 0.0;
    VideoStream awaitingStream = VideoStream::Off;  // the mode its status will have
    std::uint32_t awaitingQid = 0;                  // the qid of a seek's status; 0 otherwise
};

struct VideoInputs {
    bool connected = false;         // hello_ack has been received on this socket
    bool serverHasVideo = false;    // hello_ack features contain "video"
    double lastWantedMs = -1.0e12;  // the last time a screen showed video
    bool pinned = false;            // a run is pinned for replay
    bool playing = false;           // the replay playhead is running (not paused, not dragged)
    std::uint64_t cursorMs = 0;     // the video cursor: the replay time, or the live cursor
};

enum class VideoOp : std::uint8_t { None, SubscribeLive, SubscribeReplay, Seek, Off };

struct VideoCommand {
    VideoOp op = VideoOp::None;
    std::uint64_t fromMs = 0;  // replay start, or the seek time
    std::uint32_t qid = 0;     // a seek's qid
};

// The one request to send on this tick, if any, and the session updated to match.
VideoCommand decideVideo(const VideoInputs& in, VideoSession& session, double nowMs);

// A video_status arrived. True when it answers the request the session waits on.
bool noteVideoStatus(VideoSession& session, const ctrl2::VideoStatus& status);
// The server sent an error: the request is dropped and retried after kVideoRetryMs.
void noteVideoError(VideoSession& session, double nowMs);

}  // namespace photon::ui
