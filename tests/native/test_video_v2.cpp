// Protocol v2 video logic: piece reassembly, flag mapping, seek qids, the gap text and
// the subscription policy. The policy is driven tick by tick with the server's answers
// simulated by noteVideoStatus.
#include "photon/control_v2.hpp"
#include "photon/protocol.hpp"
#include "photon/protocol_v2.hpp"
#include "photon_ui/analysis_runtime.hpp"
#include "photon_ui/clock_format.hpp"
#include "photon_ui/video_v2.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using photon::proto2::VideoChunk;
using namespace photon::ui;
namespace c2 = photon::ctrl2;

constexpr std::size_t kPiece = photon::proto2::kVideoChunkMax;

VideoChunk piece(std::uint64_t tMs, std::uint32_t frame, std::uint8_t flags, std::uint8_t stream,
                 std::size_t size, std::uint8_t fill) {
    VideoChunk c;
    c.tMs = tMs;
    c.frame = frame;
    c.flags = flags;
    c.stream = stream;
    c.payload.assign(size, fill);
    return c;
}

constexpr std::uint8_t kF = photon::proto2::kVideoFirst;
constexpr std::uint8_t kL = photon::proto2::kVideoLast;
constexpr std::uint8_t kKey = photon::proto2::kVideoKeyframe;
constexpr std::uint8_t kCfg = photon::proto2::kVideoConfig;
constexpr std::uint8_t kRep = photon::proto2::kVideoReplay;

void testSinglePieceFrame() {
    VideoAssembler a;
    const auto out = a.push(piece(1000, 7, kF | kL | kKey, 0, 300, 0x11));
    CHECK(out.has_value());
    if (out) {
        CHECK(out->tMs == 1000);
        CHECK(out->frame == 7);
        CHECK(out->flags == kKey);  // first and last are cleared
        CHECK(out->stream == 0);
        CHECK(out->payload.size() == 300);
        CHECK(out->payload.front() == 0x11 && out->payload.back() == 0x11);
    }
    CHECK(a.dropped() == 0);
}

void testThreePieceFrame() {
    VideoAssembler a;
    CHECK(!a.push(piece(2000, 9, kF | kKey, 0, kPiece, 0xA1)).has_value());
    CHECK(!a.push(piece(2000, 9, 0, 0, kPiece, 0xA2)).has_value());
    const auto out = a.push(piece(2000, 9, kL, 0, 100, 0xA3));
    CHECK(out.has_value());
    if (out) {
        CHECK(out->payload.size() == 2 * kPiece + 100);
        CHECK(out->payload[0] == 0xA1);
        CHECK(out->payload[kPiece] == 0xA2);
        CHECK(out->payload[2 * kPiece] == 0xA3);
        CHECK(out->flags == kKey);
    }
    CHECK(a.dropped() == 0);
}

// The server cuts a frame into full pieces and drops its whole queue when it skips;
// the next piece is then the first of a new frame, so the partial one is dropped.
void testMissingMiddlePieceDropsFrame() {
    VideoAssembler a;
    CHECK(!a.push(piece(3000, 20, kF | kKey, 0, kPiece, 0xB1)).has_value());
    // The middle piece is gone; the frame's last piece does not come either.
    const auto next = a.push(piece(3033, 21, kF | kL, 0, 64, 0xB2));
    CHECK(next.has_value());
    if (next) {
        CHECK(next->frame == 21);
        CHECK(next->payload.size() == 64);
    }
    CHECK(a.dropped() == 1);
}

void testShortMiddlePieceDropsFrame() {
    VideoAssembler a;
    CHECK(!a.push(piece(4000, 30, kF, 0, kPiece, 0xC1)).has_value());
    CHECK(!a.push(piece(4000, 30, 0, 0, 100, 0xC2)).has_value());  // a middle piece is never short
    CHECK(!a.push(piece(4000, 30, kL, 0, 50, 0xC3)).has_value());  // its tail is now orphaned
    CHECK(a.dropped() == 1);
}

void testContinuationOfOtherFrameIsDropped() {
    VideoAssembler a;
    CHECK(!a.push(piece(5000, 40, kF, 0, kPiece, 0xD1)).has_value());
    CHECK(!a.push(piece(5000, 41, kL, 0, 10, 0xD2)).has_value());
    CHECK(a.dropped() == 1);
    // Nothing is held now, so a bare last piece is an orphan too.
    CHECK(!a.push(piece(5000, 41, kL, 0, 10, 0xD3)).has_value());
}

void testInterleavedStreams() {
    VideoAssembler a;
    CHECK(!a.push(piece(6000, 50, kF, 0, kPiece, 0x01)).has_value());
    CHECK(!a.push(piece(6000, 90, kF, 1, kPiece, 0x81)).has_value());
    const auto s0 = a.push(piece(6000, 50, kL, 0, 5, 0x02));
    CHECK(s0.has_value());
    if (s0) {
        CHECK(s0->stream == 0 && s0->frame == 50);
        CHECK(s0->payload.size() == kPiece + 5);
        CHECK(s0->payload[0] == 0x01 && s0->payload[kPiece] == 0x02);
    }
    const auto s1 = a.push(piece(6000, 90, kL, 1, 7, 0x82));
    CHECK(s1.has_value());
    if (s1) {
        CHECK(s1->stream == 1 && s1->frame == 90);
        CHECK(s1->payload.size() == kPiece + 7);
        CHECK(s1->payload[0] == 0x81 && s1->payload.back() == 0x82);
    }
    CHECK(a.dropped() == 0);
    // A piece of stream 1 with the first bit does not end stream 0's frame.
    VideoAssembler b;
    CHECK(!b.push(piece(6100, 1, kF, 0, kPiece, 0x10)).has_value());
    CHECK(!b.push(piece(6100, 2, kF, 1, kPiece, 0x20)).has_value());
    const auto b0 = b.push(piece(6100, 1, kL, 0, 3, 0x11));
    CHECK(b0.has_value());
    if (b0) {
        CHECK(b0->stream == 0 && b0->frame == 1);
        CHECK(b0->payload.size() == kPiece + 3);
        CHECK(b0->payload[0] == 0x10 && b0->payload[kPiece] == 0x11);
    }
    CHECK(b.dropped() == 0);
}

void testResetForgetsPartial() {
    VideoAssembler a;
    CHECK(!a.push(piece(7000, 60, kF, 0, kPiece, 0x33)).has_value());
    a.reset();
    CHECK(!a.push(piece(7000, 60, kL, 0, 4, 0x34)).has_value());
    CHECK(a.dropped() == 0);
}

void testFlagMapping() {
    CHECK(decoderFlags(0) == 0);
    CHECK(decoderFlags(kKey) == photon::proto::kVideoFlagKeyframe);
    CHECK(decoderFlags(kCfg) == photon::proto::kVideoFlagConfig);
    CHECK(decoderFlags(kKey | kCfg) == (photon::proto::kVideoFlagKeyframe | photon::proto::kVideoFlagConfig));
    // Piece bits and replay are not decoder flags.
    CHECK(decoderFlags(kF | kL | kRep) == 0);
    CHECK(!isReplayChunk(kKey | kCfg | kF | kL));
    CHECK(isReplayChunk(kRep | kKey));
}

void testSeekQidRange() {
    CHECK(kVideoQidBase == 0x40000000u);
    CHECK(isVideoQid(kVideoQidBase));
    CHECK(isVideoQid(0x7FFFFFFFu));
    CHECK(!isVideoQid(kVideoQidBase - 1));
    CHECK(!isVideoQid(photon::ui::kAnalysisQueryOrigin));
    CHECK(nextVideoQid(0) == kVideoQidBase);
    CHECK(nextVideoQid(kVideoQidBase) == kVideoQidBase + 1);
    CHECK(nextVideoQid(kVideoQidEnd - 2) == kVideoQidEnd - 1);
    CHECK(nextVideoQid(kVideoQidEnd - 1) == kVideoQidBase);  // wraps inside the range
    CHECK(nextVideoQid(5) == kVideoQidBase);                 // a tile qid is not a seek
    std::uint32_t q = 0;
    for (int i = 0; i < 100; ++i) {
        q = nextVideoQid(q);
        CHECK(isVideoQid(q));
    }
}

void testGapText() {
    char from[48];
    char to[48];
    photon::ui::formatLocalClock(std::uint64_t{1'800'000'000'000}, from, sizeof(from));
    photon::ui::formatLocalClock(std::uint64_t{1'800'000'600'000}, to, sizeof(to));

    c2::VideoGap none{1'800'000'000'000, 1'800'000'600'000, "none"};
    CHECK(videoGapText(none) == std::string("No video recorded ") + from + "\xE2\x80\x93" + to);

    c2::VideoGap noEnd{1'800'000'000'000, 0, "none"};
    CHECK(videoGapText(noEnd) == std::string("No video recorded since ") + from);

    c2::VideoGap archived{1'800'000'000'000, 1'800'000'600'000, "archived"};
    CHECK(videoGapText(archived) == "Video for this time is archived, not on the server");

    c2::VideoGap stale{1'800'000'000'000, 0, "stale"};
    CHECK(videoGapText(stale) == std::string("Camera offline since ") + from);

    c2::VideoGap unknown{0, 0, "weird"};
    CHECK(videoGapText(unknown) == "Video unavailable");
}

// --- Subscription policy -----------------------------------------------------

c2::VideoStatus statusOf(const char* mode, std::optional<std::uint32_t> qid = std::nullopt) {
    c2::VideoStatus st;
    st.mode = mode;
    st.qid = qid;
    return st;
}

VideoInputs liveIn(double wantedMs) {
    VideoInputs in;
    in.connected = true;
    in.serverHasVideo = true;
    in.lastWantedMs = wantedMs;
    return in;
}

VideoInputs replayIn(double wantedMs, std::uint64_t cursor, bool playing) {
    VideoInputs in = liveIn(wantedMs);
    in.pinned = true;
    in.playing = playing;
    in.cursorMs = cursor;
    return in;
}

void testPolicyGates() {
    VideoSession s;
    VideoInputs in = liveIn(0.0);
    in.connected = false;
    CHECK(decideVideo(in, s, 100.0).op == VideoOp::None);
    in.connected = true;
    in.serverHasVideo = false;
    CHECK(decideVideo(in, s, 100.0).op == VideoOp::None);
    in.serverHasVideo = true;
    // Nobody wants video: nothing is asked for.
    CHECK(decideVideo(liveIn(-1.0e12), s, 100.0).op == VideoOp::None);
    CHECK(s.stream == VideoStream::Off);
}

void testLiveSubscribeAndIdleOff() {
    VideoSession s;
    const double t0 = 10000.0;
    const VideoCommand sub = decideVideo(liveIn(t0), s, t0);
    CHECK(sub.op == VideoOp::SubscribeLive);
    CHECK(s.stream == VideoStream::Live);
    // Awaiting the status: nothing more is sent.
    CHECK(decideVideo(liveIn(t0), s, t0 + 10.0).op == VideoOp::None);
    CHECK(noteVideoStatus(s, statusOf("live")));
    CHECK(decideVideo(liveIn(t0 + 20.0), s, t0 + 20.0).op == VideoOp::None);  // already live

    // The debounce: still live 4.9 s after the last want, off at 5 s.
    CHECK(decideVideo(liveIn(t0), s, t0 + 4900.0).op == VideoOp::None);
    const VideoCommand off = decideVideo(liveIn(t0), s, t0 + 5000.0);
    CHECK(off.op == VideoOp::Off);
    CHECK(s.stream == VideoStream::Off);
    CHECK(noteVideoStatus(s, statusOf("off")));
    CHECK(decideVideo(liveIn(t0), s, t0 + 5100.0).op == VideoOp::None);
}

void testAwaitTimeoutAsksAgain() {
    VideoSession s;
    CHECK(decideVideo(liveIn(0.0), s, 0.0).op == VideoOp::SubscribeLive);
    CHECK(decideVideo(liveIn(2900.0), s, 2900.0).op == VideoOp::None);
    // No status came in kVideoAwaitMs: the subscribe is sent again.
    CHECK(decideVideo(liveIn(3000.0), s, 3000.0).op == VideoOp::SubscribeLive);
}

void testReplayFollowsPlayAndSeeks() {
    VideoSession s;
    const std::uint64_t c0 = 1'800'000'000'000;
    const double t0 = 1000.0;
    CHECK(decideVideo(replayIn(t0, c0, true), s, t0).op == VideoOp::SubscribeReplay);
    CHECK(s.stream == VideoStream::Replay);
    CHECK(noteVideoStatus(s, statusOf("replay")));

    // Playing at real speed: no request.
    CHECK(decideVideo(replayIn(t0 + 200.0, c0 + 200, true), s, t0 + 200.0).op == VideoOp::None);

    // A jump of 10 s is a seek, with the next qid.
    const std::uint64_t jumped = c0 + 10'000;
    const VideoCommand seek = decideVideo(replayIn(t0 + 300.0, jumped, true), s, t0 + 300.0);
    CHECK(seek.op == VideoOp::Seek);
    CHECK(seek.qid == kVideoQidBase);
    CHECK(seek.fromMs == jumped);
    CHECK(noteVideoStatus(s, statusOf("replay", kVideoQidBase)));

    // Another jump within the rate limit waits; it is sent once 250 ms have passed.
    const std::uint64_t again = jumped + 20'000;
    CHECK(decideVideo(replayIn(t0 + 350.0, again, true), s, t0 + 350.0).op == VideoOp::None);
    const VideoCommand second = decideVideo(replayIn(t0 + 560.0, again, true), s, t0 + 560.0);
    CHECK(second.op == VideoOp::Seek);
    CHECK(second.qid == kVideoQidBase + 1);
}

void testSeekStatusNeedsItsQid() {
    VideoSession s;
    const std::uint64_t c0 = 500'000;
    CHECK(decideVideo(replayIn(0.0, c0, true), s, 0.0).op == VideoOp::SubscribeReplay);
    CHECK(noteVideoStatus(s, statusOf("replay")));
    CHECK(decideVideo(replayIn(300.0, c0 + 50'000, true), s, 300.0).op == VideoOp::Seek);
    // A status for another qid, or none, does not answer the seek.
    CHECK(!noteVideoStatus(s, statusOf("replay")));
    CHECK(!noteVideoStatus(s, statusOf("replay", kVideoQidBase + 9)));
    CHECK(noteVideoStatus(s, statusOf("replay", kVideoQidBase)));
}

void testPauseStillsThenOff() {
    VideoSession s;
    const std::uint64_t c0 = 2'000'000;
    const double t0 = 0.0;
    CHECK(decideVideo(replayIn(t0, c0, true), s, t0).op == VideoOp::SubscribeReplay);
    CHECK(noteVideoStatus(s, statusOf("replay")));

    // Paused at the same cursor: the stream stays for the still window, then stops.
    CHECK(decideVideo(replayIn(1000.0, c0, false), s, 1000.0).op == VideoOp::None);
    CHECK(decideVideo(replayIn(1400.0, c0, false), s, 1400.0).op == VideoOp::None);
    const VideoCommand off = decideVideo(replayIn(1500.0, c0, false), s, 1500.0);
    CHECK(off.op == VideoOp::Off);
    CHECK(noteVideoStatus(s, statusOf("off")));
    CHECK(decideVideo(replayIn(1600.0, c0, false), s, 1600.0).op == VideoOp::None);

    // Play again from the same cursor: a new subscribe.
    CHECK(decideVideo(replayIn(1700.0, c0, true), s, 1700.0).op == VideoOp::SubscribeReplay);
}

void testScrubWhilePaused() {
    VideoSession s;
    const std::uint64_t c0 = 3'000'000;
    CHECK(decideVideo(replayIn(0.0, c0, false), s, 0.0).op == VideoOp::SubscribeReplay);
    CHECK(noteVideoStatus(s, statusOf("replay")));
    // Dragged while paused: the replay stream moves to the new cursor with a seek.
    const VideoCommand drag = decideVideo(replayIn(300.0, c0 + 40'000, false), s, 300.0);
    CHECK(drag.op == VideoOp::Seek);
    CHECK(drag.fromMs == c0 + 40'000);
    CHECK(noteVideoStatus(s, statusOf("replay", drag.qid)));
    // The stream was for a still: once the still window passes, it stops.
    CHECK(decideVideo(replayIn(2000.0, c0 + 40'000, false), s, 2000.0).op == VideoOp::Off);
}

void testServerErrorBacksOff() {
    VideoSession s;
    CHECK(decideVideo(replayIn(0.0, 7000, true), s, 0.0).op == VideoOp::SubscribeReplay);
    noteVideoError(s, 0.0);
    CHECK(s.stream == VideoStream::Off);
    CHECK(decideVideo(replayIn(0.0, 7000, true), s, 500.0).op == VideoOp::None);
    CHECK(decideVideo(replayIn(1000.0, 7000, true), s, 1000.0).op == VideoOp::SubscribeReplay);
}

void testUnpinGoesLive() {
    VideoSession s;
    CHECK(decideVideo(replayIn(0.0, 9000, true), s, 0.0).op == VideoOp::SubscribeReplay);
    CHECK(noteVideoStatus(s, statusOf("replay")));
    // Unpinned and still wanted: live replaces the replay without an off.
    const VideoCommand live = decideVideo(liveIn(100.0), s, 100.0);
    CHECK(live.op == VideoOp::SubscribeLive);
    CHECK(s.stream == VideoStream::Live);
}

void testAssemblerRejectsFrameWithoutFirst() {
    // A continuation after a dropped first piece produces nothing.
    VideoAssembler a;
    CHECK(!a.push(piece(8000, 70, kL, 0, 12, 0x44)).has_value());
    CHECK(a.dropped() == 0);
}

}  // namespace

void runVideoV2Tests() {
    testSinglePieceFrame();
    testThreePieceFrame();
    testMissingMiddlePieceDropsFrame();
    testShortMiddlePieceDropsFrame();
    testContinuationOfOtherFrameIsDropped();
    testInterleavedStreams();
    testResetForgetsPartial();
    testFlagMapping();
    testSeekQidRange();
    testGapText();
    testPolicyGates();
    testLiveSubscribeAndIdleOff();
    testAwaitTimeoutAsksAgain();
    testReplayFollowsPlayAndSeeks();
    testSeekStatusNeedsItsQid();
    testPauseStillsThenOff();
    testScrubWhilePaused();
    testServerErrorBacksOff();
    testUnpinGoesLive();
    testAssemblerRejectsFrameWithoutFirst();
}
