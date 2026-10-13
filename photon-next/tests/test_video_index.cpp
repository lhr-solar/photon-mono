#include "store/video_index.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace {

using photon::vidx::ClockProv;
using photon::vidx::CursorPlan;
using photon::vidx::CursorPlayback;
using photon::vidx::GopRing;
using photon::vidx::GopRow;
using photon::vidx::kGopFlagConfigInline;
using photon::vidx::kGopFlagContinuation;
using photon::vidx::kGopRowBytes;
using photon::vidx::LocateReply;
using photon::vidx::PlaybackMode;
using photon::vidx::SegmentCoverage;

void pushU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void pushU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void pushU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

GopRow makeRow(std::uint64_t startMs, std::uint64_t endMs, std::uint64_t offset, std::uint32_t length,
               std::uint32_t segment = 1, std::uint8_t flags = 0) {
    GopRow r{};
    r.startMs = startMs;
    r.endMs = endMs;
    r.byteOffset = offset;
    r.byteLength = length;
    r.segmentId = segment;
    r.nalCount = 60;
    r.streamId = 0;
    r.flags = flags;
    return r;
}

// The row layout is fixed by tsdb-architecture.md §8.1 and is written by
// photon-server in Go. These bytes are the layout stated independently of the
// C++ encoder, so an encoder change that the Go side did not make fails here
// rather than at a wrong seek in the field.
void testGopRowWireLayout() {
    const std::vector<std::uint8_t> wire = {
        // startMs = 0x0000019FA1B2C3D4
        0x00, 0x00, 0x01, 0x9F, 0xA1, 0xB2, 0xC3, 0xD4,
        // endMs = 0x0000019FA1B2CB9A
        0x00, 0x00, 0x01, 0x9F, 0xA1, 0xB2, 0xCB, 0x9A,
        // byteOffset = 0x0000000000807000  (8417280)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x70, 0x00,
        // byteLength = 41216
        0x00, 0x00, 0xA1, 0x00,
        // segmentId = 1412
        0x00, 0x00, 0x05, 0x84,
        // nalCount = 47
        0x00, 0x00, 0x00, 0x2F,
        // streamId, flags, reserved
        0x00, 0x01, 0x00, 0x00,
    };
    CHECK(wire.size() == kGopRowBytes);

    GopRow row{};
    CHECK(photon::vidx::decodeGopRow(wire, row));
    CHECK(row.startMs == 0x0000019FA1B2C3D4ull);
    CHECK(row.endMs == 0x0000019FA1B2CB9Aull);
    CHECK(row.byteOffset == 8417280ull);
    CHECK(row.byteLength == 41216u);
    CHECK(row.segmentId == 1412u);
    CHECK(row.nalCount == 47u);
    CHECK(row.streamId == 0);
    CHECK(row.flags == kGopFlagConfigInline);
    CHECK(photon::vidx::rowHasInlineConfig(row));
    CHECK(!photon::vidx::rowIsContinuation(row));

    std::vector<std::uint8_t> reencoded;
    photon::vidx::encodeGopRow(reencoded, row);
    CHECK(reencoded == wire);

    // A short buffer must be refused, not read past.
    GopRow partial{};
    CHECK(!photon::vidx::decodeGopRow(std::span<const std::uint8_t>(wire.data(), kGopRowBytes - 1), partial));
}

// Modes 0 and 1 are 9 bytes and must stay that way; the new modes are the only
// place the header grows (§8.2).
void testRequestFraming() {
    const auto locate = photon::vidx::encodeLocateRequest(1784843212345ull, 2, 4);
    CHECK(locate.size() == 12);
    CHECK(locate[0] == photon::vidx::kModeLocate);
    CHECK(locate[1] == 0x00);
    CHECK(locate[2] == 0x00);
    CHECK(locate[3] == 0x01);
    CHECK(locate[8] == 0x39);  // low byte of 1784843212345
    CHECK(locate[9] == 2);     // streamId
    CHECK(locate[10] == 0x00);
    CHECK(locate[11] == 0x04);  // prefetchGops

    // Layout: mode(1) startTimeMs(8) segmentId(4) offset(8) length(4).
    const auto range = photon::vidx::encodeRangeRequest(1412, 0x807000ull, 41216);
    CHECK(range.size() == 25);
    CHECK(range[0] == photon::vidx::kModeRange);
    // startTimeMs is unused and must be zero, or the server would read a span
    // from a bogus offset.
    for (std::size_t i = 1; i < 9; ++i) {
        CHECK(range[i] == 0);
    }
    CHECK(range[9] == 0x00);
    CHECK(range[10] == 0x00);
    CHECK(range[11] == 0x05);
    CHECK(range[12] == 0x84);  // segmentId 1412 = 0x584
    CHECK(range[13] == 0x00);
    CHECK(range[17] == 0x00);
    CHECK(range[18] == 0x80);
    CHECK(range[19] == 0x70);
    CHECK(range[20] == 0x00);  // offset 0x807000
    CHECK(range[21] == 0x00);
    CHECK(range[22] == 0x00);
    CHECK(range[23] == 0xA1);
    CHECK(range[24] == 0x00);  // length 41216 = 0xA100

    const auto replay = photon::vidx::encodeReplayRangeRequest(1000, 5000);
    CHECK(replay.size() == 17);
    CHECK(replay[0] == photon::vidx::kModeReplayRange);
    CHECK(replay[8] == 0xE8);   // 1000
    CHECK(replay[16] == 0x88);  // 5000

    // The extended modes must not collide with the legacy ones.
    CHECK(photon::vidx::kModeLive == 0);
    CHECK(photon::vidx::kModeReplay == 1);
    CHECK(photon::vidx::kModeReplayRange == 2);
    CHECK(photon::vidx::kModeLocate == 3);
    CHECK(photon::vidx::kModeRange == 4);
    CHECK(photon::vidx::kModeClock == 5);

    const auto clockReq = photon::vidx::encodeClockRequest(-137);
    CHECK(clockReq.size() == 9);
    CHECK(clockReq[0] == photon::vidx::kModeClock);
    std::int64_t gotOff = 0;
    auto gotProv = photon::vidx::ClockProv::Receive;
    std::vector<std::uint8_t> clockBody;
    // i64be -137 + prov rtcp
    const std::uint64_t bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(-137));
    for (int i = 7; i >= 0; --i) {
        clockBody.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
    }
    clockBody.push_back(3);
    CHECK(photon::vidx::decodeClockReply(clockBody, gotOff, gotProv));
    CHECK(gotOff == -137);
    CHECK(gotProv == photon::vidx::ClockProv::Rtcp);
}

std::vector<std::uint8_t> buildLocatePayload(std::uint8_t status, std::int64_t clockOffsetMs,
                                             std::uint8_t prov, std::int64_t residualMs,
                                             const std::vector<GopRow>& rows,
                                             const std::vector<SegmentCoverage>& coverage,
                                             const std::vector<std::uint8_t>& sps,
                                             const std::vector<std::uint8_t>& pps) {
    std::vector<std::uint8_t> body;
    body.push_back(status);
    pushU64(body, static_cast<std::uint64_t>(clockOffsetMs));
    body.push_back(prov);
    pushU64(body, static_cast<std::uint64_t>(residualMs));
    pushU32(body, static_cast<std::uint32_t>(rows.size()));  // totalRows
    pushU64(body, rows.empty() ? 0 : rows.front().startMs);
    pushU64(body, rows.empty() ? 0 : rows.back().endMs);
    pushU16(body, static_cast<std::uint16_t>(sps.size()));
    body.insert(body.end(), sps.begin(), sps.end());
    pushU16(body, static_cast<std::uint16_t>(pps.size()));
    body.insert(body.end(), pps.begin(), pps.end());
    pushU16(body, static_cast<std::uint16_t>(rows.size()));
    for (const GopRow& r : rows) {
        photon::vidx::encodeGopRow(body, r);
    }
    pushU16(body, static_cast<std::uint16_t>(coverage.size()));
    for (const SegmentCoverage& c : coverage) {
        pushU32(body, c.segmentId);
        pushU64(body, c.startMs);
        pushU64(body, c.endMs);
    }
    return body;
}

void testLocateReplyDecode() {
    const std::vector<GopRow> rows = {
        makeRow(1784843212000ull, 1784843213966ull, 0, 40960, 1412, kGopFlagConfigInline),
        makeRow(1784843214000ull, 1784843215966ull, 40960, 38000, 1412),
    };
    const std::vector<SegmentCoverage> cov = {
        SegmentCoverage{1412, 1784843212000ull, 1784843215966ull},
    };
    const std::vector<std::uint8_t> sps = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E};
    const std::vector<std::uint8_t> pps = {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80};

    // A negative offset is the interesting case: it rides an unsigned field.
    const auto payload = buildLocatePayload(0, -137, static_cast<std::uint8_t>(ClockProv::Rtcp), -250, rows,
                                            cov, sps, pps);
    LocateReply reply;
    CHECK(photon::vidx::decodeLocateReply(payload, reply));
    CHECK(reply.found);
    CHECK(reply.clockOffsetMs == -137);
    CHECK(reply.provenance == ClockProv::Rtcp);
    CHECK(photon::vidx::clockVerified(reply.provenance));
    CHECK(reply.residualMs == -250);
    CHECK(reply.totalRows == 2);
    CHECK(reply.rows.size() == 2);
    CHECK(reply.rows[0].startMs == 1784843212000ull);
    CHECK(reply.rows[0].byteLength == 40960u);
    CHECK(photon::vidx::rowHasInlineConfig(reply.rows[0]));
    CHECK(reply.rows[1].startMs == 1784843214000ull);
    CHECK(reply.coverage.size() == 1);
    CHECK(reply.coverage[0].segmentId == 1412u);
    CHECK(reply.sps == sps);
    CHECK(reply.pps == pps);

    // Not-found still carries the clock story, because a client needs to know
    // whether A/V sync is verified even where there is no video.
    const auto missing = buildLocatePayload(1, 12, static_cast<std::uint8_t>(ClockProv::Manual), 0, {}, {},
                                            {}, {});
    LocateReply none;
    CHECK(photon::vidx::decodeLocateReply(missing, none));
    CHECK(!none.found);
    CHECK(none.clockOffsetMs == 12);
    CHECK(none.provenance == ClockProv::Manual);
    CHECK(!photon::vidx::clockVerified(none.provenance));
    CHECK(none.rows.empty());

    // Truncation anywhere must be rejected rather than silently yielding a
    // half-built reply.
    for (std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{20}, payload.size() - 1}) {
        LocateReply bad;
        CHECK(!photon::vidx::decodeLocateReply(std::span<const std::uint8_t>(payload.data(), cut), bad));
    }
    // Trailing bytes mean the layouts have drifted apart.
    auto extra = payload;
    extra.push_back(0xFF);
    LocateReply drifted;
    CHECK(!photon::vidx::decodeLocateReply(extra, drifted));
}

void testClockOffsetConversion() {
    // Positive offset: video time runs behind telemetry time.
    CHECK(photon::vidx::videoMsForCursor(10'500, 500) == 10'000);
    CHECK(photon::vidx::cursorMsForVideo(10'000, 500) == 10'500);
    // Negative offset.
    CHECK(photon::vidx::videoMsForCursor(10'000, -500) == 10'500);
    CHECK(photon::vidx::cursorMsForVideo(10'500, -500) == 10'000);
    // Round trip at a realistic magnitude.
    CHECK(photon::vidx::cursorMsForVideo(photon::vidx::videoMsForCursor(1784843212345ull, -137), -137) ==
          1784843212345ull);
    // Saturating rather than wrapping: a small cursor with a large positive
    // offset must not become a timestamp in the far future.
    CHECK(photon::vidx::videoMsForCursor(100, 5'000) == 0);
    CHECK(photon::vidx::cursorMsForVideo(100, -5'000) == 0);
}

void testEntryRowSelection() {
    std::vector<GopRow> rows;
    for (int i = 0; i < 8; ++i) {
        const auto start = static_cast<std::uint64_t>(10'000 + i * 2000);
        rows.push_back(makeRow(start, start + 1966, static_cast<std::uint64_t>(i) * 4096, 4096));
    }

    struct Case {
        std::uint64_t at;
        std::ptrdiff_t want;
    };
    const Case cases[] = {
        {10'000, 0}, {10'001, 0}, {11'999, 0}, {12'000, 1},
        {13'500, 1}, {23'999, 6}, {24'000, 7}, {99'999, 7},
        {1, 0},
    };
    for (const Case& c : cases) {
        const auto got = photon::vidx::entryRowFor(rows, 0, c.at);
        CHECK(got == c.want);
        if (got >= 0 && c.at >= rows.front().startMs) {
            // The entry must be the *last* keyframe at or before the cursor.
            CHECK(rows[static_cast<std::size_t>(got)].startMs <= c.at);
            const auto next = static_cast<std::size_t>(got) + 1;
            CHECK(next >= rows.size() || rows[next].startMs > c.at);
        }
    }

    // A continuation row is not a decode entry point.
    std::vector<GopRow> split = {
        makeRow(1000, 1033, 0, 60, 1),
        makeRow(1066, 1066, 0, 20, 2, kGopFlagContinuation),
    };
    CHECK(photon::vidx::entryRowFor(split, 0, 1066) == 0);

    CHECK(photon::vidx::entryRowFor(rows, 3, 12'000) == -1);
    CHECK(photon::vidx::entryRowFor({}, 0, 12'000) == -1);
}

// §9.8: "Dragging issues no requests at all — not a reduced number, not coarse
// ones, none." That is the property most easily lost, so assert it directly.
void testScrubIssuesNoRequests() {
    // The hardest case first: a drag with nothing resident at all. It is
    // tempting to "just fetch one pointer" here, and that is exactly the
    // per-position round trip §9.8 rules out.
    CursorPlayback bare;
    for (std::uint64_t t = 0; t <= 300'000; t += 911) {
        const CursorPlan plan = bare.update(t, PlaybackMode::Scrubbing);
        CHECK(!plan.requestLocate);
        CHECK(!plan.haveTarget);
    }
    CHECK(bare.locateRequests() == 0);

    CursorPlayback pb;
    LocateReply reply;
    reply.found = true;
    reply.clockOffsetMs = 0;
    for (int i = 0; i < 4; ++i) {
        const auto start = static_cast<std::uint64_t>(100'000 + i * 2000);
        reply.rows.push_back(makeRow(start, start + 1966, static_cast<std::uint64_t>(i) * 4096, 4096));
    }
    pb.applyLocate(reply);
    // A locate reply resolves whatever request was pending, so start from a
    // clean count.
    const std::uint32_t before = pb.locateRequests();

    // Drag across the whole resident range and well past both ends.
    for (std::uint64_t t = 50'000; t <= 200'000; t += 137) {
        const CursorPlan plan = pb.update(t, PlaybackMode::Scrubbing);
        CHECK(!plan.requestLocate);
        CHECK(!plan.keyframeOnly);
    }
    CHECK(pb.locateRequests() == before);
}

void testScrubDecodesToCursorWhenResident() {
    CursorPlayback pb;
    LocateReply reply;
    reply.found = true;
    for (int i = 0; i < 4; ++i) {
        const auto start = static_cast<std::uint64_t>(100'000 + i * 2000);
        reply.rows.push_back(makeRow(start, start + 1966, static_cast<std::uint64_t>(i) * 4096, 4096));
    }
    pb.applyLocate(reply);

    // Mid-GOP cursor: P-frames already in the ring are released up to the
    // playhead, not the GOP's IDR. That is what made a 1-minute pin a slideshow.
    CursorPlan plan = pb.update(103'500, PlaybackMode::Scrubbing);
    CHECK(plan.haveTarget);
    CHECK(plan.target.startMs == 102'000);
    CHECK(!plan.keyframeOnly);
    CHECK(plan.feedUpToMs == 103'500);

    // Moving inside the same GOP must not re-enter the decoder.
    plan = pb.update(103'700, PlaybackMode::Scrubbing);
    CHECK(!plan.reenter);
    CHECK(plan.target.startMs == 102'000);
    CHECK(plan.feedUpToMs == 103'700);

    // Crossing into the next GOP must.
    plan = pb.update(104'100, PlaybackMode::Scrubbing);
    CHECK(plan.reenter);
    CHECK(plan.target.startMs == 104'000);
    CHECK(plan.feedUpToMs == 104'100);
}

void testReplayPrefetchCoversAMinute() {
    CHECK(photon::vidx::replayPrefetchGops(0, 0) == 8);
    CHECK(photon::vidx::replayPrefetchGops(1'000, 2'000) == 4);
    const auto minute = photon::vidx::replayPrefetchGops(1'789'244'940'000ull, 1'789'245'000'000ull);
    CHECK(minute == 4);
    CHECK(photon::vidx::replayPrefetchGops(0, 3'600'000ull) == 4);
}

// §8.3: exact decode happens on release, in the Committed state.
void testCommitDecodesToTheExactCursor() {
    CursorPlayback pb;
    LocateReply reply;
    reply.found = true;
    for (int i = 0; i < 4; ++i) {
        const auto start = static_cast<std::uint64_t>(100'000 + i * 2000);
        reply.rows.push_back(makeRow(start, start + 1966, static_cast<std::uint64_t>(i) * 4096, 4096));
    }
    pb.applyLocate(reply);

    const CursorPlan plan = pb.update(103'500, PlaybackMode::Committed);
    CHECK(plan.haveTarget);
    CHECK(plan.target.startMs == 102'000);
    CHECK(!plan.keyframeOnly);
    CHECK(plan.feedUpToMs == 103'500);
    // Inside a resident GOP, a commit needs no round trip either.
    CHECK(!plan.requestLocate);
}

void testCommitPrefetchesBeforeLastGopEnds() {
    CursorPlayback pb;
    LocateReply reply;
    reply.found = true;
    reply.rows.push_back(makeRow(100'000, 101'966, 0, 4096));
    pb.applyLocate(reply);

    CursorPlan early = pb.update(100'100, PlaybackMode::Committed);
    CHECK(!early.requestLocate);

    CursorPlan late = pb.update(100'500, PlaybackMode::Committed);
    CHECK(late.requestLocate);
    CHECK(late.locateAtMs == 101'967);
    const CursorPlan again = pb.update(101'700, PlaybackMode::Committed);
    CHECK(!again.requestLocate);
}

void testCommitRequestsPointerOutsideResidentRange() {
    CursorPlayback pb;
    LocateReply reply;
    reply.found = true;
    reply.rows.push_back(makeRow(100'000, 101'966, 0, 4096));
    pb.applyLocate(reply);

    // Well past the resident GOP: one request, and exactly one until answered.
    CursorPlan plan = pb.update(500'000, PlaybackMode::Committed);
    CHECK(plan.requestLocate);
    CHECK(plan.locateAtMs == 500'000);
    const std::uint32_t after = pb.locateRequests();
    for (int i = 0; i < 20; ++i) {
        const CursorPlan repeat = pb.update(500'000 + static_cast<std::uint64_t>(i), PlaybackMode::Committed);
        CHECK(!repeat.requestLocate);
    }
    CHECK(pb.locateRequests() == after);

    // The reply lands and covers the cursor; no further request.
    LocateReply second;
    second.found = true;
    second.rows.push_back(makeRow(499'000, 500'966, 8192, 4096));
    second.rows.push_back(makeRow(501'000, 502'966, 12288, 4096));
    pb.applyLocate(second);
    plan = pb.update(500'000, PlaybackMode::Committed);
    CHECK(!plan.requestLocate);
    CHECK(plan.target.startMs == 499'000);
    CHECK(pb.covers(500'000));
}

// The whole point of §8.5: if the offset is not applied, video and telemetry
// disagree by exactly the offset and nothing in the UI says so.
void testClockOffsetSteersTheTarget() {
    CursorPlayback pb;
    LocateReply reply;
    reply.found = true;
    reply.clockOffsetMs = 500;  // video timestamps + 500 = telemetry time
    reply.provenance = ClockProv::Rtcp;
    reply.residualMs = -250;
    reply.rows.push_back(makeRow(100'000, 101'966, 0, 4096));
    reply.rows.push_back(makeRow(102'000, 103'966, 4096, 4096));
    pb.applyLocate(reply);
    CHECK(pb.clockOffsetMs() == 500);
    CHECK(pb.provenance() == ClockProv::Rtcp);
    CHECK(pb.residualMs() == -250);

    // Telemetry cursor 102'400 is video time 101'900, which is still inside the
    // first GOP. Ignoring the offset would pick the second.
    const CursorPlan plan = pb.update(102'400, PlaybackMode::Committed);
    CHECK(plan.haveTarget);
    CHECK(plan.target.startMs == 100'000);
    CHECK(plan.feedUpToMs == 101'900);

    // And a cursor 500 ms later crosses into the second GOP.
    const CursorPlan next = pb.update(102'500, PlaybackMode::Committed);
    CHECK(next.target.startMs == 102'000);
    CHECK(next.reenter);
}

// Pacing: video advances because the cursor advanced, not because time passed.
void testFrameDuePacing() {
    CursorPlayback pb;
    LocateReply reply;
    reply.found = true;
    reply.clockOffsetMs = 500;
    reply.rows.push_back(makeRow(100'000, 101'966, 0, 4096));
    pb.applyLocate(reply);

    // Video frame at 101'000 sits at telemetry 101'500.
    CHECK(!pb.frameDue(101'000, 101'499));
    CHECK(pb.frameDue(101'000, 101'500));
    CHECK(pb.frameDue(101'000, 200'000));
    // A frame ahead of the cursor is withheld, which is what stops video from
    // running away on its own clock.
    CHECK(!pb.frameDue(101'900, 101'500));
}

void testIdleAndLiveModes() {
    CursorPlayback pb;
    LocateReply reply;
    reply.found = true;
    reply.rows.push_back(makeRow(100'000, 101'966, 0, 4096));
    pb.applyLocate(reply);

    const CursorPlan idle = pb.update(100'500, PlaybackMode::Idle);
    CHECK(!idle.requestLocate);
    CHECK(!idle.haveTarget);
    CHECK(idle.feedUpToMs == 0);

    // Live must not gate on the cursor at all: the newest bytes are correct.
    const CursorPlan live = pb.update(100'500, PlaybackMode::Live);
    CHECK(!live.requestLocate);
    CHECK(live.feedUpToMs == UINT64_MAX);
    CHECK(!live.keyframeOnly);
}

void testCoverageQuery() {
    CursorPlayback pb;
    LocateReply reply;
    reply.found = true;
    reply.rows.push_back(makeRow(100'000, 101'966, 0, 4096));
    reply.coverage.push_back(SegmentCoverage{1, 100'000, 140'000});
    reply.coverage.push_back(SegmentCoverage{2, 200'000, 240'000});
    pb.applyLocate(reply);

    CHECK(pb.covered(120'000));
    CHECK(pb.covered(220'000));
    // The gap between segments is a real gap, and the scrubber has to be able
    // to shade it rather than the user discovering it by landing in it.
    CHECK(!pb.covered(170'000));
    CHECK(!pb.covered(10'000));
    CHECK(pb.coverage().size() == 2);
    CHECK(photon::vidx::firstCoveredVideoMs(pb.coverage(), 120'000, 140'000) == 120'000);
    CHECK(photon::vidx::firstCoveredVideoMs(pb.coverage(), 90'000, 250'000) == 100'000);
    CHECK(photon::vidx::firstCoveredVideoMs(pb.coverage(), 150'000, 190'000) == 0);
    CHECK(photon::vidx::firstCoveredVideoMs(pb.coverage(), 180'000, 0) == 200'000);
}

void testClipLocateCoverageAndEncodeRoundTrip() {
    LocateReply fat;
    fat.found = true;
    fat.minMs = 1;
    fat.maxMs = 100;
    fat.totalRows = 1;
    fat.rows.push_back(makeRow(50, 60, 0, 100));
    for (int i = 0; i < 80; ++i) {
        fat.coverage.push_back(SegmentCoverage{static_cast<std::uint32_t>(i + 1),
                                               static_cast<std::uint64_t>(i) * 1000,
                                               static_cast<std::uint64_t>(i) * 1000 + 500});
    }
    LocateReply live = fat;
    photon::vidx::clipLocateCoverage(live, 0);
    CHECK(live.coverage.size() == 24);
    CHECK(live.coverage.front().segmentId == 57u);

    LocateReply mid = fat;
    photon::vidx::clipLocateCoverage(mid, 40'200);
    CHECK(mid.coverage.size() == 80);
    LocateReply longRun = fat;
    for (int i = 80; i < 200; ++i) {
        longRun.coverage.push_back(SegmentCoverage{static_cast<std::uint32_t>(i + 1),
                                                   static_cast<std::uint64_t>(i) * 1000,
                                                   static_cast<std::uint64_t>(i) * 1000 + 500});
    }
    photon::vidx::clipLocateCoverage(longRun, 40'200);
    CHECK(longRun.coverage.size() <= 128);
    CHECK(longRun.coverage.front().startMs <= 40'200);
    CHECK(longRun.coverage.back().endMs >= 40'200);

    const auto wire = photon::vidx::encodeLocateReply(fat);
    LocateReply back;
    CHECK(photon::vidx::decodeLocateReply(wire, back));
    CHECK(back.found);
    CHECK(back.rows.size() == 1);
    CHECK(back.rows[0].startMs == 50);
    CHECK(back.coverage.size() == 80);
    CHECK(back.coverage[10].segmentId == 11u);
}

// Rows accumulate so a small scrub stays inside what is already resident, but
// an open GOP's row grows, so a newer copy has to replace the older one rather
// than sitting beside it.
void testRowAccumulation() {
    CursorPlayback pb;
    LocateReply first;
    first.found = true;
    first.rows.push_back(makeRow(100'000, 100'500, 0, 1024));
    pb.applyLocate(first);
    CHECK(pb.rows().size() == 1);
    CHECK(pb.rows()[0].endMs == 100'500);

    LocateReply grown;
    grown.found = true;
    grown.rows.push_back(makeRow(100'000, 101'966, 0, 4096));
    grown.rows.push_back(makeRow(102'000, 103'966, 4096, 4096));
    pb.applyLocate(grown);
    CHECK(pb.rows().size() == 2);
    CHECK(pb.rows()[0].endMs == 101'966);
    CHECK(pb.rows()[0].byteLength == 4096u);
    CHECK(pb.rows()[1].startMs == 102'000);

    // Sorted by startMs regardless of arrival order, since entry lookup walks
    // them.
    LocateReply earlier;
    earlier.found = true;
    earlier.rows.push_back(makeRow(98'000, 99'966, 0, 4096, 1));
    pb.applyLocate(earlier);
    CHECK(pb.rows().size() == 3);
    CHECK(pb.rows()[0].startMs == 98'000);
    CHECK(pb.rows()[1].startMs == 100'000);
    CHECK(pb.rows()[2].startMs == 102'000);

    pb.clear();
    CHECK(pb.rows().empty());
}

// §9.12: cache compressed GOP bytes, never decoded frames, bounded so it cannot
// grow with session length.
void testGopRingBoundsAndKeyframeSearch() {
    GopRing ring;
    ring.setBudget(4096);

    std::vector<std::uint8_t> keyNal(1000, 0x65);
    keyNal[1] = 0x80; // first_mb_in_slice == 0
    const std::vector<std::uint8_t> deltaNal(300, 0x41);

    // 10 GOPs of 1 key + 3 delta at 2 s spacing: 19 kB against a 4 kB budget.
    for (int g = 0; g < 10; ++g) {
        const auto base = static_cast<std::uint64_t>(100'000 + g * 2000);
        ring.push(base, 1 /* keyframe */, keyNal);
        for (int k = 1; k <= 3; ++k) {
            ring.push(base + static_cast<std::uint64_t>(k) * 33, 0, deltaNal);
        }
    }
    CHECK(ring.bytes() <= ring.budget());
    CHECK(ring.size() > 0);
    // Newest data survives; oldest is evicted.
    CHECK(ring.newestMs() == 100'000 + 9 * 2000 + 99);
    CHECK(ring.oldestMs() > 100'000);

    // The newest keyframe at or before a time is the only re-entry point.
    const auto idx = ring.keyframeAtOrBefore(118'050);
    CHECK(idx >= 0);
    CHECK(ring.nals()[static_cast<std::size_t>(idx)].videoMs == 118'000);
    CHECK((ring.nals()[static_cast<std::size_t>(idx)].flags & 1) != 0);

    // Before every resident keyframe there is no re-entry point, and claiming
    // one would feed the decoder a delta frame with no reference.
    CHECK(ring.keyframeAtOrBefore(1'000) == -1);

    GopRing empty;
    CHECK(empty.keyframeAtOrBefore(5) == -1);
    CHECK(empty.bytes() == 0);

    // A single NAL larger than the budget must still be retained: dropping it
    // would leave the ring unable to ever hold a keyframe.
    GopRing tiny;
    tiny.setBudget(64);
    tiny.push(1, 1, keyNal);
    CHECK(tiny.size() == 1);
    tiny.push(2, 0, deltaNal);
    CHECK(tiny.size() == 1);
    CHECK(tiny.nals()[0].videoMs == 2);
}

void testReplayWindowDropsOtherDaysGop() {
    CursorPlayback pb;
    pb.setReplayWindow(200'000, 240'000);

    LocateReply wrongDay;
    wrongDay.found = true;
    wrongDay.clockOffsetMs = 0;
    wrongDay.rows.push_back(makeRow(100'000, 101'966, 0, 4096, 1));
    wrongDay.coverage.push_back(SegmentCoverage{1, 100'000, 140'000});
    wrongDay.coverage.push_back(SegmentCoverage{2, 200'000, 240'000});
    pb.applyLocate(wrongDay);
    CHECK(pb.rows().empty());
    CHECK(pb.coverage().size() == 1);
    CHECK(pb.coverage()[0].segmentId == 2);
    CHECK(pb.firstReplayCoveredMs() == 200'000);
    CHECK(!pb.videoInReplayWindow(100'500));
    CHECK(pb.videoInReplayWindow(210'000));

    LocateReply rightDay;
    rightDay.found = true;
    rightDay.clockOffsetMs = 0;
    rightDay.rows.push_back(makeRow(200'000, 201'966, 8192, 4096, 2));
    pb.applyLocate(rightDay);
    CHECK(pb.rows().size() == 1);
    CHECK(pb.rows()[0].startMs == 200'000);
    CHECK(pb.covers(201'000));
}

}  // namespace

void runVideoIndexTests() {
    {
        GopRing ring;
        ring.push(1000, 1, std::vector<std::uint8_t>{0,0,1,0x65,0x80});
        ring.push(1000, 1, std::vector<std::uint8_t>{0,0,1,0x65,0x40});
        ring.push(1000, 1, std::vector<std::uint8_t>{0,0,1,0x65,0x60});
        CHECK(ring.keyframeAtOrBefore(1000) == 0);
    }
    {
        CursorPlayback pb;
        CHECK(pb.update(500000, PlaybackMode::Committed).requestLocate);
        pb.applyLocate(LocateReply{});
        pb.markMissingVideoAt(500000);
        for (int i = 0; i < 100; ++i) {
            CHECK(!pb.update(500000, PlaybackMode::Committed).requestLocate);
        }
        CHECK(pb.update(501000, PlaybackMode::Committed).requestLocate);
        pb.clear();
        CHECK(!pb.missingVideoAt(500000));
    }
    testGopRowWireLayout();
    testRequestFraming();
    testLocateReplyDecode();
    testClockOffsetConversion();
    testEntryRowSelection();
    testScrubIssuesNoRequests();
    testScrubDecodesToCursorWhenResident();
    testReplayPrefetchCoversAMinute();
    testCommitDecodesToTheExactCursor();
    testCommitPrefetchesBeforeLastGopEnds();
    testCommitRequestsPointerOutsideResidentRange();
    testClockOffsetSteersTheTarget();
    testFrameDuePacing();
    testIdleAndLiveModes();
    testCoverageQuery();
    testRowAccumulation();
    testGopRingBoundsAndKeyframeSearch();
    testReplayWindowDropsOtherDaysGop();
    testClipLocateCoverageAndEncodeRoundTrip();
}
