#include "photon/video_index.hpp"

#include "photon/endian.hpp"

#include <algorithm>
#include <limits>

namespace photon::vidx {
namespace {

// photon-server writes its on-disk and wire video formats big-endian, so the
// pointer table and the locate reply follow the same rule.
bool readU16(std::span<const std::uint8_t> in, std::size_t& off, std::uint16_t& v) {
    if (off + 2 > in.size()) {
        return false;
    }
    v = static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[off]) << 8) | in[off + 1]);
    off += 2;
    return true;
}

bool readU32(std::span<const std::uint8_t> in, std::size_t& off, std::uint32_t& v) {
    if (off + 4 > in.size()) {
        return false;
    }
    v = (static_cast<std::uint32_t>(in[off]) << 24) | (static_cast<std::uint32_t>(in[off + 1]) << 16) |
        (static_cast<std::uint32_t>(in[off + 2]) << 8) | static_cast<std::uint32_t>(in[off + 3]);
    off += 4;
    return true;
}

bool readU64(std::span<const std::uint8_t> in, std::size_t& off, std::uint64_t& v) {
    if (off + 8 > in.size()) {
        return false;
    }
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(in[off + static_cast<std::size_t>(i)]);
    }
    off += 8;
    return true;
}

bool readU8(std::span<const std::uint8_t> in, std::size_t& off, std::uint8_t& v) {
    if (off + 1 > in.size()) {
        return false;
    }
    v = in[off];
    off += 1;
    return true;
}

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

// A NAL flags bit0 = keyframe, matching photon-server's VideoFlagKeyframe and
// the protocol's kVideoFlagKeyframe.
constexpr std::uint8_t kNalFlagKeyframe = 1 << 0;
constexpr std::uint8_t kNalFlagConfig = 1 << 1;

}  // namespace

const char* clockProvName(ClockProv p) {
    switch (p) {
        case ClockProv::Rtcp:
            return "rtcp";
        case ClockProv::Ntp:
            return "ntp";
        case ClockProv::Manual:
            return "manual";
        case ClockProv::Receive:
            break;
    }
    return "receive";
}

std::uint64_t videoMsForCursor(std::uint64_t cursorMs, std::int64_t offsetMs) {
    if (offsetMs >= 0) {
        const auto off = static_cast<std::uint64_t>(offsetMs);
        return cursorMs > off ? cursorMs - off : 0;
    }
    return cursorMs + static_cast<std::uint64_t>(-offsetMs);
}

std::uint64_t cursorMsForVideo(std::uint64_t videoMs, std::int64_t offsetMs) {
    if (offsetMs >= 0) {
        return videoMs + static_cast<std::uint64_t>(offsetMs);
    }
    const auto off = static_cast<std::uint64_t>(-offsetMs);
    return videoMs > off ? videoMs - off : 0;
}

void encodeGopRow(std::vector<std::uint8_t>& out, const GopRow& row) {
    pushU64(out, row.startMs);
    pushU64(out, row.endMs);
    pushU64(out, row.byteOffset);
    pushU32(out, row.byteLength);
    pushU32(out, row.segmentId);
    pushU32(out, row.nalCount);
    out.push_back(row.streamId);
    out.push_back(row.flags);
    pushU16(out, 0);
}

bool decodeGopRow(std::span<const std::uint8_t> bytes, GopRow& row) {
    if (bytes.size() < kGopRowBytes) {
        return false;
    }
    std::size_t off = 0;
    std::uint16_t reserved = 0;
    const bool ok = readU64(bytes, off, row.startMs) && readU64(bytes, off, row.endMs) &&
                    readU64(bytes, off, row.byteOffset) && readU32(bytes, off, row.byteLength) &&
                    readU32(bytes, off, row.segmentId) && readU32(bytes, off, row.nalCount) &&
                    readU8(bytes, off, row.streamId) && readU8(bytes, off, row.flags) &&
                    readU16(bytes, off, reserved);
    return ok && off == kGopRowBytes;
}

std::vector<std::uint8_t> encodeReplayRangeRequest(std::uint64_t startMs, std::uint64_t endMs) {
    std::vector<std::uint8_t> out;
    out.push_back(kModeReplayRange);
    pushU64(out, startMs);
    pushU64(out, endMs);
    return out;
}

std::vector<std::uint8_t> encodeLocateRequest(std::uint64_t atMs, std::uint8_t streamId,
                                             std::uint16_t prefetchGops) {
    std::vector<std::uint8_t> out;
    out.push_back(kModeLocate);
    pushU64(out, atMs);
    out.push_back(streamId);
    pushU16(out, prefetchGops);
    return out;
}

std::vector<std::uint8_t> encodeRangeRequest(std::uint32_t segmentId, std::uint64_t offset,
                                            std::uint32_t length) {
    std::vector<std::uint8_t> out;
    out.push_back(kModeRange);
    // startTimeMs is unused for a range read; the span is named explicitly.
    pushU64(out, 0);
    pushU32(out, segmentId);
    pushU64(out, offset);
    pushU32(out, length);
    return out;
}

std::vector<std::uint8_t> encodeClockRequest(std::int64_t offsetMs) {
    std::vector<std::uint8_t> out;
    out.push_back(kModeClock);
    pushU64(out, static_cast<std::uint64_t>(offsetMs));
    return out;
}

bool decodeClockReply(std::span<const std::uint8_t> payload, std::int64_t& offsetMs, ClockProv& provenance) {
    if (payload.size() < 9) {
        return false;
    }
    std::size_t off = 0;
    std::uint64_t bits = 0;
    std::uint8_t prov = 0;
    if (!readU64(payload, off, bits) || !readU8(payload, off, prov)) {
        return false;
    }
    offsetMs = static_cast<std::int64_t>(bits);
    provenance = prov > static_cast<std::uint8_t>(ClockProv::Rtcp) ? ClockProv::Receive
                                                                  : static_cast<ClockProv>(prov);
    return true;
}

bool decodeLocateReply(std::span<const std::uint8_t> payload, LocateReply& out) {
    out = LocateReply{};
    std::size_t off = 0;
    std::uint8_t status = 0;
    std::uint64_t clockOffset = 0;
    std::uint8_t prov = 0;
    std::uint64_t residual = 0;
    if (!readU8(payload, off, status) || !readU64(payload, off, clockOffset) || !readU8(payload, off, prov) ||
        !readU64(payload, off, residual) || !readU32(payload, off, out.totalRows) ||
        !readU64(payload, off, out.minMs) || !readU64(payload, off, out.maxMs)) {
        return false;
    }
    out.found = status == 0;
    out.clockOffsetMs = static_cast<std::int64_t>(clockOffset);
    out.residualMs = static_cast<std::int64_t>(residual);
    out.provenance = prov > static_cast<std::uint8_t>(ClockProv::Rtcp) ? ClockProv::Receive
                                                                      : static_cast<ClockProv>(prov);

    const auto readBlob = [&](std::vector<std::uint8_t>& dest) {
        std::uint16_t len = 0;
        if (!readU16(payload, off, len)) {
            return false;
        }
        if (off + len > payload.size()) {
            return false;
        }
        dest.assign(payload.begin() + static_cast<std::ptrdiff_t>(off),
                    payload.begin() + static_cast<std::ptrdiff_t>(off + len));
        off += len;
        return true;
    };
    if (!readBlob(out.sps) || !readBlob(out.pps)) {
        return false;
    }

    std::uint16_t rowCount = 0;
    if (!readU16(payload, off, rowCount)) {
        return false;
    }
    out.rows.reserve(rowCount);
    for (std::uint16_t i = 0; i < rowCount; ++i) {
        if (off + kGopRowBytes > payload.size()) {
            return false;
        }
        GopRow row{};
        if (!decodeGopRow(payload.subspan(off, kGopRowBytes), row)) {
            return false;
        }
        out.rows.push_back(row);
        off += kGopRowBytes;
    }

    std::uint16_t covCount = 0;
    if (!readU16(payload, off, covCount)) {
        return false;
    }
    out.coverage.reserve(covCount);
    for (std::uint16_t i = 0; i < covCount; ++i) {
        SegmentCoverage cov{};
        if (!readU32(payload, off, cov.segmentId) || !readU64(payload, off, cov.startMs) ||
            !readU64(payload, off, cov.endMs)) {
            return false;
        }
        out.coverage.push_back(cov);
    }
    // Trailing bytes mean the two sides disagree about the layout, which is
    // exactly the kind of drift that otherwise shows up as a wrong seek.
    return off == payload.size();
}

std::vector<std::uint8_t> encodeLocateReply(const LocateReply& reply) {
    std::vector<std::uint8_t> body;
    body.reserve(64 + reply.rows.size() * kGopRowBytes + reply.coverage.size() * 20 + reply.sps.size() +
                 reply.pps.size());
    body.push_back(reply.found ? 0 : 1);
    pushU64(body, static_cast<std::uint64_t>(reply.clockOffsetMs));
    body.push_back(static_cast<std::uint8_t>(reply.provenance));
    pushU64(body, static_cast<std::uint64_t>(reply.residualMs));
    pushU32(body, reply.totalRows);
    pushU64(body, reply.minMs);
    pushU64(body, reply.maxMs);
    pushU16(body, static_cast<std::uint16_t>(reply.sps.size()));
    body.insert(body.end(), reply.sps.begin(), reply.sps.end());
    pushU16(body, static_cast<std::uint16_t>(reply.pps.size()));
    body.insert(body.end(), reply.pps.begin(), reply.pps.end());
    pushU16(body, static_cast<std::uint16_t>(reply.rows.size()));
    for (const auto& row : reply.rows) {
        encodeGopRow(body, row);
    }
    pushU16(body, static_cast<std::uint16_t>(reply.coverage.size()));
    for (const auto& c : reply.coverage) {
        pushU32(body, c.segmentId);
        pushU64(body, c.startMs);
        pushU64(body, c.endMs);
    }
    return body;
}

void clipLocateCoverage(LocateReply& reply, std::uint64_t atMs) {
    constexpr std::size_t kLiveMax = 24;
    constexpr std::size_t kReplayMax = 128;
    auto& cov = reply.coverage;
    if (atMs == 0) {
        if (cov.size() > kLiveMax) {
            cov.erase(cov.begin(), cov.end() - static_cast<std::ptrdiff_t>(kLiveMax));
        }
        return;
    }
    if (cov.size() <= kReplayMax) {
        return;
    }
    const std::uint64_t lo = atMs > 15ull * 60ull * 1000ull ? atMs - 15ull * 60ull * 1000ull : 0;
    const std::uint64_t hi = atMs + 15ull * 60ull * 1000ull;
    std::vector<SegmentCoverage> keep;
    keep.reserve(kReplayMax);
    for (const auto& c : cov) {
        if (c.endMs < lo || c.startMs > hi) {
            continue;
        }
        keep.push_back(c);
    }
    if (keep.empty()) {
        keep.assign(cov.end() - static_cast<std::ptrdiff_t>(std::min(kReplayMax, cov.size())), cov.end());
    } else if (keep.size() > kReplayMax) {
        std::size_t best = 0;
        std::uint64_t bestDist = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t i = 0; i < keep.size(); ++i) {
            const std::uint64_t mid = keep[i].startMs + (keep[i].endMs - keep[i].startMs) / 2;
            const std::uint64_t dist = mid > atMs ? mid - atMs : atMs - mid;
            if (dist < bestDist) {
                bestDist = dist;
                best = i;
            }
        }
        const std::size_t begin = best > kReplayMax / 2 ? best - kReplayMax / 2 : 0;
        const std::size_t end = std::min(keep.size(), begin + kReplayMax);
        keep.assign(keep.begin() + static_cast<std::ptrdiff_t>(begin),
                    keep.begin() + static_cast<std::ptrdiff_t>(end));
    }
    cov = std::move(keep);
}

std::ptrdiff_t entryRowFor(std::span<const GopRow> rows, std::uint8_t streamId, std::uint64_t videoMs) {
    std::ptrdiff_t best = -1;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const GopRow& r = rows[i];
        if (r.streamId != streamId || rowIsContinuation(r)) {
            continue;
        }
        if (r.startMs > videoMs) {
            continue;
        }
        if (best < 0 || r.startMs > rows[static_cast<std::size_t>(best)].startMs) {
            best = static_cast<std::ptrdiff_t>(i);
        }
    }
    if (best >= 0) {
        return best;
    }
    // videoMs is before every keyframe. The earliest one is the only place a
    // decoder can start, so hand that back rather than nothing.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].streamId == streamId && !rowIsContinuation(rows[i])) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// CursorPlayback
// ---------------------------------------------------------------------------

void CursorPlayback::addRows(std::span<const GopRow> rows) {
    for (const GopRow& row : rows) {
        const auto it = std::lower_bound(rows_.begin(), rows_.end(), row,
                                         [](const GopRow& a, const GopRow& b) {
                                             if (a.streamId != b.streamId) {
                                                 return a.streamId < b.streamId;
                                             }
                                             return a.startMs < b.startMs;
                                         });
        if (it != rows_.end() && it->streamId == row.streamId && it->startMs == row.startMs) {
            // A row can grow while its GOP is still open, so the newer copy wins.
            *it = row;
            continue;
        }
        rows_.insert(it, row);
    }
}

void CursorPlayback::setReplayWindow(std::uint64_t minCursorMs, std::uint64_t maxCursorMs) {
    replayWindowOn_ = true;
    replayMinCursorMs_ = minCursorMs;
    replayMaxCursorMs_ = maxCursorMs;
}

void CursorPlayback::clearReplayWindow() {
    replayWindowOn_ = false;
    replayMinCursorMs_ = 0;
    replayMaxCursorMs_ = 0;
}

bool CursorPlayback::videoInReplayWindow(std::uint64_t videoMs) const {
    if (!replayWindowOn_) {
        return true;
    }
    const std::uint64_t minV = videoMsForCursor(replayMinCursorMs_, clockOffsetMs_);
    const std::uint64_t maxV =
        replayMaxCursorMs_ == 0 ? 0 : videoMsForCursor(replayMaxCursorMs_, clockOffsetMs_);
    return videoOverlapsWindow(videoMs, videoMs, minV, maxV);
}

std::uint64_t CursorPlayback::firstReplayCoveredMs() const {
    if (!replayWindowOn_) {
        return 0;
    }
    const std::uint64_t minV = videoMsForCursor(replayMinCursorMs_, clockOffsetMs_);
    const std::uint64_t maxV =
        replayMaxCursorMs_ == 0 ? 0 : videoMsForCursor(replayMaxCursorMs_, clockOffsetMs_);
    return firstCoveredVideoMs(coverage_, minV, maxV);
}

void CursorPlayback::applyLocate(const LocateReply& reply) {
    locatePending_ = false;
    clockOffsetMs_ = reply.clockOffsetMs;
    provenance_ = reply.provenance;
    residualMs_ = reply.residualMs;
    if (!reply.sps.empty()) {
        sps_ = reply.sps;
    }
    if (!reply.pps.empty()) {
        pps_ = reply.pps;
    }
    if (!reply.coverage.empty()) {
        coverage_ = reply.coverage;
    }
    if (replayWindowOn_ && !coverage_.empty()) {
        const std::uint64_t minV = videoMsForCursor(replayMinCursorMs_, clockOffsetMs_);
        const std::uint64_t maxV =
            replayMaxCursorMs_ == 0 ? 0 : videoMsForCursor(replayMaxCursorMs_, clockOffsetMs_);
        std::vector<SegmentCoverage> clipped;
        clipped.reserve(coverage_.size());
        for (const auto& c : coverage_) {
            if (videoOverlapsWindow(c.startMs, c.endMs, minV, maxV)) {
                clipped.push_back(c);
            }
        }
        coverage_ = std::move(clipped);
    }
    if (!reply.found) {
        return;
    }
    if (!replayWindowOn_) {
        addRows(reply.rows);
        return;
    }
    const std::uint64_t minV = videoMsForCursor(replayMinCursorMs_, clockOffsetMs_);
    const std::uint64_t maxV =
        replayMaxCursorMs_ == 0 ? 0 : videoMsForCursor(replayMaxCursorMs_, clockOffsetMs_);
    std::vector<GopRow> keep;
    keep.reserve(reply.rows.size());
    for (const auto& row : reply.rows) {
        if (videoOverlapsWindow(row.startMs, row.endMs, minV, maxV)) {
            keep.push_back(row);
        }
    }
    if (!keep.empty()) {
        addRows(keep);
    }
}

void CursorPlayback::clear() {
    rows_.clear();
    coverage_.clear();
    sps_.clear();
    pps_.clear();
    entryStartMs_ = 0;
    haveEntry_ = false;
    locatePending_ = false;
    lastLocateAtMs_ = 0;
    residualMs_ = 0;
    replayWindowOn_ = false;
    replayMinCursorMs_ = 0;
    replayMaxCursorMs_ = 0;
}

bool CursorPlayback::covers(std::uint64_t cursorMs) const {
    const std::uint64_t videoMs = videoMsForCursor(cursorMs, clockOffsetMs_);
    for (const GopRow& r : rows_) {
        if (r.streamId != streamId_ || rowIsContinuation(r)) {
            continue;
        }
        if (videoMs >= r.startMs && videoMs <= r.endMs) {
            return true;
        }
    }
    return false;
}

bool CursorPlayback::covered(std::uint64_t cursorMs) const {
    const std::uint64_t videoMs = videoMsForCursor(cursorMs, clockOffsetMs_);
    for (const SegmentCoverage& c : coverage_) {
        if (videoMs >= c.startMs && videoMs <= c.endMs) {
            return true;
        }
    }
    return false;
}

bool CursorPlayback::frameDue(std::uint64_t frameVideoMs, std::uint64_t cursorMs) const {
    return cursorMsForVideo(frameVideoMs, clockOffsetMs_) <= cursorMs;
}

CursorPlan CursorPlayback::update(std::uint64_t cursorMs, PlaybackMode mode) {
    CursorPlan plan;
    if (mode == PlaybackMode::Idle) {
        return plan;
    }
    if (mode == PlaybackMode::Live) {
        // Live has no cursor to follow: the newest bytes are the right bytes,
        // and the existing live path already paces them.
        plan.feedUpToMs = std::numeric_limits<std::uint64_t>::max();
        haveEntry_ = false;
        return plan;
    }

    const std::uint64_t videoMs = videoMsForCursor(cursorMs, clockOffsetMs_);
    const std::ptrdiff_t idx = entryRowFor(rows_, streamId_, videoMs);

    if (idx >= 0) {
        plan.haveTarget = true;
        plan.target = rows_[static_cast<std::size_t>(idx)];
        plan.reenter = !haveEntry_ || plan.target.startMs != entryStartMs_;
        entryStartMs_ = plan.target.startMs;
        haveEntry_ = true;
    }

    if (mode == PlaybackMode::Scrubbing) {
        // §9.8: a drag issues zero backend round trips. Decode P-frames that
        // are already in the ring up to the cursor so a 1-minute pin is 30 fps
        // instead of one IDR per GOP. A GOP that is not resident stays on
        // its keyframe (feedUpToMs = start) rather than fetching.
        plan.keyframeOnly = false;
        plan.feedUpToMs = plan.haveTarget ? videoMs : 0;
        return plan;
    }

    // Committed: exact decode up to the cursor, and a pointer request when the
    // cursor is somewhere no resident row covers. Also prefetch before the
    // last resident GOP ends so Play does not freeze on one still.
    plan.feedUpToMs = videoMs;
    const bool inside = idx >= 0 && videoMs >= plan.target.startMs && videoMs <= plan.target.endMs;
    bool haveLater = false;
    if (inside) {
        for (const GopRow& r : rows_) {
            if (r.streamId != streamId_ || rowIsContinuation(r)) {
                continue;
            }
            if (r.startMs > plan.target.startMs) {
                haveLater = true;
                break;
            }
        }
    }
    // haveLater is "a later pointer exists", not "those bytes are in the
    // ring". Still ask before this GOP ends so Play does not sit on a still
    // when the first locate only shipped one GOP of NALs.
    const bool nearEnd = inside && plan.target.endMs > plan.target.startMs &&
                         videoMs + kLocateLeadMs >= plan.target.endMs && !haveLater;
    if ((!inside || nearEnd) && !locatePending_) {
        plan.requestLocate = true;
        plan.locateAtMs = nearEnd ? plan.target.endMs + 1 : videoMs;
        lastLocateAtMs_ = plan.locateAtMs;
        locatePending_ = true;
        ++locateRequests_;
    }
    return plan;
}

// ---------------------------------------------------------------------------
// GopRing
// ---------------------------------------------------------------------------

void GopRing::clear() {
    nals_.clear();
    bytes_ = 0;
}

void GopRing::push(std::uint64_t videoMs, std::uint8_t flags, std::span<const std::uint8_t> nal) {
    BufferedNal buf;
    buf.videoMs = videoMs;
    buf.flags = flags;
    buf.seq = nextSeq_++;
    buf.nal.assign(nal.begin(), nal.end());
    bytes_ += buf.nal.size();
    nals_.push_back(std::move(buf));

    std::size_t drop = 0;
    while (bytes_ > budget_ && drop + 1 < nals_.size()) {
        bytes_ -= nals_[drop].nal.size();
        ++drop;
    }
    if (drop > 0) {
        nals_.erase(nals_.begin(), nals_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

std::ptrdiff_t GopRing::keyframeAtOrBefore(std::uint64_t videoMs) const {
    std::ptrdiff_t best = -1;
    for (std::size_t i = 0; i < nals_.size(); ++i) {
        if ((nals_[i].flags & kNalFlagKeyframe) == 0) {
            continue;
        }
        if (nals_[i].videoMs > videoMs) {
            break;
        }
        best = static_cast<std::ptrdiff_t>(i);
    }
    (void)kNalFlagConfig;
    return best;
}

}  // namespace photon::vidx
