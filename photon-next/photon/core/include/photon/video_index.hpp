#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Video pointer table and cursor-driven playback (docs/pdr/tsdb-architecture.md
// §8).
//
// The pointer table itself is written by photon-server, live as video arrives.
// This module is the reading half: the row and reply codecs that mirror the
// server's byte layout exactly, and the playback logic that makes video a
// follower of the telemetry cursor rather than a second timeline with its own
// clock and its own Go button.
//
// Everything here is pure logic with no I/O, so the playback state machine is
// testable natively instead of only inside a browser.
namespace photon::vidx {

// ---------------------------------------------------------------------------
// Pointer rows (§8.1)
// ---------------------------------------------------------------------------

inline constexpr std::size_t kGopRowBytes = 40;

// bit0: the byte span begins with SPS/PPS, so a decoder can be configured from
// this range alone.
inline constexpr std::uint8_t kGopFlagConfigInline = 1 << 0;
// bit1: the row does not begin at a keyframe. A GOP split across a segment
// boundary produces one of these. It is a legal byte range to fetch but not a
// legal place to start decoding.
inline constexpr std::uint8_t kGopFlagContinuation = 1 << 1;

struct GopRow {
    std::uint64_t startMs = 0;
    std::uint64_t endMs = 0;
    std::uint64_t byteOffset = 0;
    std::uint32_t byteLength = 0;
    std::uint32_t segmentId = 0;
    std::uint32_t nalCount = 0;
    std::uint8_t streamId = 0;
    std::uint8_t flags = 0;
};

inline bool rowIsContinuation(const GopRow& r) { return (r.flags & kGopFlagContinuation) != 0; }
inline bool rowHasInlineConfig(const GopRow& r) { return (r.flags & kGopFlagConfigInline) != 0; }

void encodeGopRow(std::vector<std::uint8_t>& out, const GopRow& row);
bool decodeGopRow(std::span<const std::uint8_t> bytes, GopRow& row);

// ---------------------------------------------------------------------------
// Clock provenance (§8.5)
// ---------------------------------------------------------------------------

// Ordered worst to best so "rtcp or better" is a comparison. §8.5: treat any
// claim of A/V sync as unverified below Rtcp.
enum class ClockProv : std::uint8_t {
    Receive = 0,
    Manual = 1,
    Ntp = 2,
    Rtcp = 3,
};

inline bool clockVerified(ClockProv p) { return p >= ClockProv::Rtcp; }
const char* clockProvName(ClockProv p);

// Telemetry time and video time differ by the recorded offset. Conversions are
// saturating at zero: a cursor early in a session with a positive offset would
// otherwise wrap into the far future.
std::uint64_t videoMsForCursor(std::uint64_t cursorMs, std::int64_t offsetMs);
std::uint64_t cursorMsForVideo(std::uint64_t videoMs, std::int64_t offsetMs);

// ---------------------------------------------------------------------------
// photon-server :6700 request framing
// ---------------------------------------------------------------------------

// Modes 0 and 1 keep their exact 9-byte header, so this only ever adds.
inline constexpr std::uint8_t kModeLive = 0;
inline constexpr std::uint8_t kModeReplay = 1;
inline constexpr std::uint8_t kModeReplayRange = 2;
inline constexpr std::uint8_t kModeLocate = 3;
inline constexpr std::uint8_t kModeRange = 4;
inline constexpr std::uint8_t kModeClock = 5;

std::vector<std::uint8_t> encodeReplayRangeRequest(std::uint64_t startMs, std::uint64_t endMs);
std::vector<std::uint8_t> encodeLocateRequest(std::uint64_t atMs, std::uint8_t streamId,
                                              std::uint16_t prefetchGops);
std::vector<std::uint8_t> encodeRangeRequest(std::uint32_t segmentId, std::uint64_t offset,
                                             std::uint32_t length);
// Mode 5: persist the manual A/V nudge. `offsetMs` is two's-complement in the
// 8-byte startTimeMs field so negative values stay on the existing 9-byte header.
std::vector<std::uint8_t> encodeClockRequest(std::int64_t offsetMs);
bool decodeClockReply(std::span<const std::uint8_t> payload, std::int64_t& offsetMs, ClockProv& provenance);

// ---------------------------------------------------------------------------
// video_locate reply
// ---------------------------------------------------------------------------

struct SegmentCoverage {
    std::uint32_t segmentId = 0;
    std::uint64_t startMs = 0;
    std::uint64_t endMs = 0;
};

// First video timestamp in [fromMs, toMs] that coverage says exists.
// 0 if the window does not overlap any recorded segment. toMs == 0 is unbounded.
inline std::uint64_t firstCoveredVideoMs(std::span<const SegmentCoverage> coverage, std::uint64_t fromMs,
                                         std::uint64_t toMs) {
    std::uint64_t best = 0;
    bool have = false;
    for (const auto& c : coverage) {
        if (c.endMs < fromMs) {
            continue;
        }
        if (toMs != 0 && c.startMs > toMs) {
            continue;
        }
        const std::uint64_t snap = c.startMs < fromMs ? fromMs : c.startMs;
        if ((toMs != 0 && snap > toMs) || snap > c.endMs) {
            continue;
        }
        if (!have || snap < best) {
            best = snap;
            have = true;
        }
    }
    return have ? best : 0;
}

struct LocateReply {
    bool found = false;
    // Add this to a stored video timestamp to get a telemetry timestamp.
    std::int64_t clockOffsetMs = 0;
    ClockProv provenance = ClockProv::Receive;
    // Derived wall clock minus raw receive time for the entry keyframe: the
    // network delay plus clock disagreement the mapping removed.
    std::int64_t residualMs = 0;
    std::uint32_t totalRows = 0;
    std::uint64_t minMs = 0;
    std::uint64_t maxMs = 0;
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
    // Entry GOP first, then the prefetch run.
    std::vector<GopRow> rows;
    std::vector<SegmentCoverage> coverage;
};

// `payload` excludes the u32 length prefix the server writes ahead of it.
bool decodeLocateReply(std::span<const std::uint8_t> payload, LocateReply& out);
std::vector<std::uint8_t> encodeLocateReply(const LocateReply& reply);
// Live locates were shipping thousands of coverage rows (~60 kB) every poll.
void clipLocateCoverage(LocateReply& reply, std::uint64_t atMs);

// Index of the GOP a video timestamp lands in, or -1. Continuation rows are
// never returned: a decoder cannot start on one.
std::ptrdiff_t entryRowFor(std::span<const GopRow> rows, std::uint8_t streamId, std::uint64_t videoMs);

// ---------------------------------------------------------------------------
// Cursor-driven playback (§8.3)
// ---------------------------------------------------------------------------

enum class PlaybackMode : std::uint8_t {
    // Nothing is asking for video.
    Idle,
    // A drag is in progress. §9.8: zero backend round trips, so this never
    // asks for a pointer. Resident P-frames are released up to the cursor
    // (same as Committed); a GOP that is not in the ring stays on its
    // keyframe rather than fetching.
    Scrubbing,
    // A cursor was committed. Exact decode happens here.
    Committed,
    // Follow the live tail rather than the cursor.
    Live,
};

struct CursorPlan {
    // Ask the backend for a pointer at locateAtMs (video time).
    bool requestLocate = false;
    std::uint64_t locateAtMs = 0;

    bool haveTarget = false;
    GopRow target{};

    // The entry GOP changed, so the decoder has to be fed from a keyframe
    // rather than continuing where it was.
    bool reenter = false;
    // Feed only the target GOP's keyframe and stop.
    bool keyframeOnly = false;
    // Feed frames whose video timestamp is at or before this.
    std::uint64_t feedUpToMs = 0;
};

// Ask for the next GOP this far before the current one ends, so Play does not
// stall on a 1.5 s locate after the last picture.
inline constexpr std::uint64_t kLocateLeadMs = 400;
// Same hold as photon-server videoLocateHoldMs. A GOP further from the
// pinned session than this is another day's clip, not "keep the last still".
inline constexpr std::uint64_t kReplayHoldMs = 2500;
// Live ring cap. A pinned replay uses kReplayCacheBudget.
inline constexpr std::size_t kLiveCacheBudget = 8 * 1024 * 1024;
// Compressed-NAL budget for a pinned replay window (~1 minute of 720p).
inline constexpr std::size_t kReplayCacheBudget = 16 * 1024 * 1024;
// First locate inside a pinned window. Play asks again as the ring's newest
// timestamp is approached, until the replay budget is full.
inline constexpr std::uint16_t kReplayLocateGops = 4;
// Historical locate when no replay window is pinned.
inline constexpr std::uint16_t kHistoryLocateGops = 8;

inline bool videoOverlapsWindow(std::uint64_t startMs, std::uint64_t endMs, std::uint64_t minV,
                                std::uint64_t maxV) {
    if (maxV != 0 && startMs > maxV + kReplayHoldMs) {
        return false;
    }
    if (minV != 0 && endMs + kReplayHoldMs < minV) {
        return false;
    }
    return true;
}

// Decides what the decoder should be fed, given where the cursor is.
//
// This holds no bytes and does no I/O: it is the policy half, so that the
// "video follows the cursor" rule is a thing that can be asserted rather than
// an emergent property of UI code.
class CursorPlayback {
public:
    void setStreamId(std::uint8_t streamId) { streamId_ = streamId; }
    std::uint8_t streamId() const { return streamId_; }

    // Rows accumulate across locates so a small scrub stays inside what is
    // already resident. Rows are kept sorted and de-duplicated by startMs.
    void addRows(std::span<const GopRow> rows);
    void applyLocate(const LocateReply& reply);
    void cancelPendingLocate() { locatePending_ = false; }
    void clear();
    // Catalog pin: only keep GOPs that overlap this telemetry window.
    // t=0 locate is "newest live GOP" and must not be used while this is on.
    void setReplayWindow(std::uint64_t minCursorMs, std::uint64_t maxCursorMs);
    void clearReplayWindow();
    bool replayWindowOn() const { return replayWindowOn_; }
    std::uint64_t replayMinCursorMs() const { return replayMinCursorMs_; }
    std::uint64_t replayMaxCursorMs() const { return replayMaxCursorMs_; }
    bool videoInReplayWindow(std::uint64_t videoMs) const;
    std::uint64_t firstReplayCoveredMs() const;

    std::span<const GopRow> rows() const { return rows_; }
    std::int64_t clockOffsetMs() const { return clockOffsetMs_; }
    void setClockOffsetMs(std::int64_t ms) { clockOffsetMs_ = ms; }
    ClockProv provenance() const { return provenance_; }
    std::int64_t residualMs() const { return residualMs_; }
    const std::vector<std::uint8_t>& sps() const { return sps_; }
    const std::vector<std::uint8_t>& pps() const { return pps_; }
    std::span<const SegmentCoverage> coverage() const { return coverage_; }

    // True when a cursor position is inside a resident, decodable GOP.
    bool covers(std::uint64_t cursorMs) const;
    // True when video exists at this cursor position according to the coarse
    // coverage map, so the scrubber can shade gaps instead of the user finding
    // them by landing in one.
    bool covered(std::uint64_t cursorMs) const;

    CursorPlan update(std::uint64_t cursorMs, PlaybackMode mode);

    // Pacing: a frame is due once the cursor has reached it. This is what
    // replaces video's own clock.
    bool frameDue(std::uint64_t frameVideoMs, std::uint64_t cursorMs) const;

    std::uint64_t lastLocateAtMs() const { return lastLocateAtMs_; }
    std::uint64_t entryStartMs() const { return entryStartMs_; }
    std::uint32_t locateRequests() const { return locateRequests_; }

private:
    std::vector<GopRow> rows_;
    std::vector<SegmentCoverage> coverage_;
    std::vector<std::uint8_t> sps_;
    std::vector<std::uint8_t> pps_;
    std::int64_t clockOffsetMs_ = 0;
    std::int64_t residualMs_ = 0;
    ClockProv provenance_ = ClockProv::Receive;
    std::uint8_t streamId_ = 0;
    std::uint64_t entryStartMs_ = 0;
    bool haveEntry_ = false;
    std::uint64_t lastLocateAtMs_ = 0;
    bool locatePending_ = false;
    std::uint32_t locateRequests_ = 0;
    bool replayWindowOn_ = false;
    std::uint64_t replayMinCursorMs_ = 0;
    std::uint64_t replayMaxCursorMs_ = 0;
};

// ---------------------------------------------------------------------------
// GOP ring (§9.12)
// ---------------------------------------------------------------------------

struct BufferedNal {
    std::uint64_t videoMs = 0;
    std::uint8_t flags = 0;
    // Monotonic across the ring's lifetime. A caller tracking "what have I fed
    // the decoder" cannot use an index, because eviction renumbers them, and
    // cannot use the timestamp, because a keyframe and its SPS/PPS share one.
    std::uint64_t seq = 0;
    std::vector<std::uint8_t> nal;
};

// Compressed NALs held around the cursor, never decoded frames.
//
// A ±30 s window is about 1.2 MB at 40 kB/GOP, which is what lets a small
// scrub replay without touching the network. Bounded by bytes, oldest-first,
// so it cannot grow with session length.
class GopRing {
public:
    static constexpr std::size_t kDefaultBudget = kLiveCacheBudget;

    void setBudget(std::size_t bytes) {
        budget_ = bytes == 0 ? kDefaultBudget : bytes;
        std::size_t drop = 0;
        while (bytes_ > budget_ && drop + 1 < nals_.size()) {
            bytes_ -= nals_[drop].nal.size();
            ++drop;
        }
        if (drop > 0) {
            nals_.erase(nals_.begin(), nals_.begin() + static_cast<std::ptrdiff_t>(drop));
        }
    }
    std::size_t budget() const { return budget_; }
    std::size_t bytes() const { return bytes_; }
    std::size_t size() const { return nals_.size(); }
    void clear();

    void push(std::uint64_t videoMs, std::uint8_t flags, std::span<const std::uint8_t> nal);

    const std::vector<BufferedNal>& nals() const { return nals_; }

    // Index of the newest keyframe at or before videoMs, or -1. This is the
    // only position a decoder can be (re-)entered at.
    std::ptrdiff_t keyframeAtOrBefore(std::uint64_t videoMs) const;

    std::uint64_t oldestMs() const { return nals_.empty() ? 0 : nals_.front().videoMs; }
    std::uint64_t newestMs() const { return nals_.empty() ? 0 : nals_.back().videoMs; }
    std::uint64_t nextSeq() const { return nextSeq_; }

private:
    std::vector<BufferedNal> nals_;
    std::size_t bytes_ = 0;
    std::size_t budget_ = kDefaultBudget;
    std::uint64_t nextSeq_ = 1;
};

}  // namespace photon::vidx
