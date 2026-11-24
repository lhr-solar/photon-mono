#include "photon/protocol_v2.hpp"
#include "test_harness.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace p2 = photon::proto2;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Span = std::span<const std::uint8_t>;

constexpr std::int64_t kMinI64 = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMaxI64 = std::numeric_limits<std::int64_t>::max();
constexpr std::uint32_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();

p2::LiveSamples sampleLive() {
    p2::LiveSamples m;
    m.baseNs = kMinI64;
    m.records = {
        {0u, 0u, 0.0},
        {kMaxU32, kMaxU32, -1.5},
        {1000u, 17u, std::numeric_limits<double>::quiet_NaN()},
    };
    return m;
}

p2::Gap sampleGap() {
    p2::Gap m;
    m.fromNs = 1'800'000'000'000'000'000;
    m.toNs = kMaxI64;
    return m;
}

p2::TilePiece sampleTileLevel0() {
    p2::TilePiece p;
    p.qid = kMaxU32;
    p.signal = 7;
    p.level = 0;
    p.flags = static_cast<std::uint8_t>(p2::kTileSealed | p2::kTileLast);
    p.index = -123456789012345;
    p.offset = 4000;
    p.n = 3;
    p.tMs = {1.8e12, 1.8e12 + 0.5, 1.8e12 + 10.0};
    p.value = {1.5, -2.25, 0.0};
    return p;
}

p2::TilePiece sampleTileLevel2() {
    p2::TilePiece p;
    p.qid = 9;
    p.signal = 3;
    p.level = 2;
    p.flags = 0;
    p.index = kMaxI64;
    p.offset = 0;
    p.n = 2;
    p.tMs = {100.0, 200.0};
    p.min = {-40.0, 0.125};
    p.max = {-0.5, 3.75};
    p.mean = {-20.25, 1.0};
    return p;
}

p2::VideoChunk sampleVideo() {
    p2::VideoChunk c;
    c.tMs = std::numeric_limits<std::uint64_t>::max();
    c.frame = kMaxU32;
    c.flags = static_cast<std::uint8_t>(p2::kVideoKeyframe | p2::kVideoFirst | p2::kVideoLast);
    c.stream = 255;
    for (std::size_t i = 0; i < 256; ++i) {
        c.payload.push_back(static_cast<std::uint8_t>(i));
    }
    return c;
}

// Each prefix shorter than the frame must be refused.
template <class Decode>
void expectPrefixesRejected(const Bytes& frame, Decode decodeOk) {
    int accepted = 0;
    for (std::size_t n = 0; n < frame.size(); ++n) {
        if (decodeOk(Span(frame.data(), n))) {
            ++accepted;
        }
    }
    CHECK(accepted == 0);
    CHECK(decodeOk(Span(frame)));
}

void testHeaderFields() {
    const Bytes frame = p2::encodeGap(sampleGap(), 0x11223344u);
    CHECK(frame.size() == 28);
    const auto decoded = p2::decodeFrame(frame);
    CHECK(decoded.has_value());
    if (!decoded) {
        return;
    }
    CHECK(decoded->header.type == 0x21);
    CHECK(decoded->header.seq == 0x11223344u);
    CHECK(decoded->header.len == 16);
    CHECK(decoded->payload.size() == 16);
    CHECK(frame[0] == 0x50);
    CHECK(frame[1] == 0x32);
    CHECK(frame[2] == 2);
    CHECK(frame[3] == 0x21);
    CHECK(frame[4] == 0x44);
    CHECK(frame[7] == 0x11);
}

void testLiveSamplesRoundTrip() {
    const p2::LiveSamples msg = sampleLive();
    const Bytes frame = p2::encodeLiveSamples(msg, 42);
    CHECK(frame.size() == 12 + 12 + 3 * 16);
    const auto back = p2::decodeLiveSamples(frame);
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    CHECK(back->baseNs == kMinI64);
    CHECK(back->records.size() == 3);
    CHECK(back->records[0].dtUs == 0);
    CHECK(back->records[0].signal == 0);
    CHECK(back->records[0].value == 0.0);
    CHECK(back->records[1].dtUs == kMaxU32);
    CHECK(back->records[1].signal == kMaxU32);
    CHECK(back->records[1].value == -1.5);
    CHECK(std::isnan(back->records[2].value));
    CHECK(back->records[2].dtUs == 1000);
    CHECK(back->records[2].signal == 17);

    const auto header = p2::decodeFrame(frame);
    CHECK(header.has_value());
    CHECK(header->header.type == 0x20);
    CHECK(header->header.seq == 42);
}

void testLiveSamplesEmptyAndBaseExtremes() {
    p2::LiveSamples msg;
    msg.baseNs = kMaxI64;
    const Bytes frame = p2::encodeLiveSamples(msg);
    CHECK(frame.size() == 24);
    const auto back = p2::decodeLiveSamples(frame);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->baseNs == kMaxI64);
        CHECK(back->records.empty());
    }
}

void testGapRoundTrip() {
    const p2::Gap msg = sampleGap();
    const Bytes frame = p2::encodeGap(msg);
    CHECK(frame.size() == 28);
    const auto back = p2::decodeGap(frame);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->fromNs == 1'800'000'000'000'000'000);
        CHECK(back->toNs == kMaxI64);
    }

    p2::Gap low;
    low.fromNs = kMinI64;
    low.toNs = 1'800'000'000'123;
    const auto lowBack = p2::decodeGap(p2::encodeGap(low));
    CHECK(lowBack.has_value());
    if (lowBack) {
        CHECK(lowBack->fromNs == kMinI64);
        CHECK(lowBack->toNs == 1'800'000'000'123);
    }
}

void testTileLevel0RoundTrip() {
    const p2::TilePiece piece = sampleTileLevel0();
    const Bytes frame = p2::encodeTile(piece, 5);
    CHECK(frame.size() == 12 + 28 + 2 * 8 * 3);
    const auto back = p2::decodeTile(frame);
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    CHECK(back->qid == kMaxU32);
    CHECK(back->signal == 7);
    CHECK(back->level == 0);
    CHECK(back->flags == (p2::kTileSealed | p2::kTileLast));
    CHECK(back->index == -123456789012345);
    CHECK(back->offset == 4000);
    CHECK(back->n == 3);
    CHECK(back->tMs == piece.tMs);
    CHECK(back->value == piece.value);
    CHECK(back->min.empty());
    CHECK(back->max.empty());
    CHECK(back->mean.empty());
}

void testTileLevelAboveZeroRoundTrip() {
    const p2::TilePiece piece = sampleTileLevel2();
    const Bytes frame = p2::encodeTile(piece);
    CHECK(frame.size() == 12 + 28 + 4 * 8 * 2);
    const auto back = p2::decodeTile(frame);
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    CHECK(back->level == 2);
    CHECK(back->index == kMaxI64);
    CHECK(back->n == 2);
    CHECK(back->tMs == piece.tMs);
    CHECK(back->min == piece.min);
    CHECK(back->max == piece.max);
    CHECK(back->mean == piece.mean);
    CHECK(back->value.empty());
}

void testTileIndexExtremes() {
    p2::TilePiece piece = sampleTileLevel0();
    piece.index = kMinI64;
    piece.n = 0;
    piece.tMs.clear();
    piece.value.clear();
    const auto back = p2::decodeTile(p2::encodeTile(piece));
    CHECK(back.has_value());
    if (back) {
        CHECK(back->index == kMinI64);
        CHECK(back->n == 0);
        CHECK(back->tMs.empty());
    }
}

void testTileShortArraysArePadded() {
    // encodeTile writes n = tMs.size() values per array; a short array is padded.
    p2::TilePiece piece = sampleTileLevel0();
    piece.value = {9.0};
    const auto back = p2::decodeTile(p2::encodeTile(piece));
    CHECK(back.has_value());
    if (back) {
        CHECK(back->n == 3);
        CHECK(back->value.size() == 3);
        CHECK(back->value[0] == 9.0);
        CHECK(back->value[1] == 0.0);
        CHECK(back->value[2] == 0.0);
    }
}

void testVideoChunkRoundTrip() {
    const p2::VideoChunk chunk = sampleVideo();
    const Bytes frame = p2::encodeVideoChunk(chunk, 3);
    CHECK(frame.size() == 12 + 16 + 256);
    CHECK(frame[26] == 0);  // reserved bytes are written as zero
    CHECK(frame[27] == 0);
    const auto back = p2::decodeVideoChunk(frame);
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    CHECK(back->tMs == std::numeric_limits<std::uint64_t>::max());
    CHECK(back->frame == kMaxU32);
    CHECK(back->flags == (p2::kVideoKeyframe | p2::kVideoFirst | p2::kVideoLast));
    CHECK(back->stream == 255);
    CHECK(back->payload == chunk.payload);

    p2::VideoChunk empty;
    empty.tMs = 1;
    const auto emptyBack = p2::decodeVideoChunk(p2::encodeVideoChunk(empty));
    CHECK(emptyBack.has_value());
    if (emptyBack) {
        CHECK(emptyBack->payload.empty());
        CHECK(emptyBack->tMs == 1);
    }
}

void testHandBuiltGap() {
    // Gap from_ns 0x0102030405060708, to_ns 0x1112131415161718, seq 7.
    const Bytes expected = {
        0x50, 0x32, 0x02, 0x21,                          // magic "P2", version 2, type 0x21
        0x07, 0x00, 0x00, 0x00,                          // seq
        0x10, 0x00, 0x00, 0x00,                          // len 16
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // from_ns
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,  // to_ns
    };
    p2::Gap gap;
    gap.fromNs = 0x0102030405060708;
    gap.toNs = 0x1112131415161718;
    CHECK(p2::encodeGap(gap, 7) == expected);
    const auto back = p2::decodeGap(expected);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->fromNs == 0x0102030405060708);
        CHECK(back->toNs == 0x1112131415161718);
    }
}

void testHandBuiltLiveSamples() {
    // base_ns 1, one record: dt_us 2, signal 3, value 1.0.
    const Bytes expected = {
        0x50, 0x32, 0x02, 0x20,                          // magic, version, type 0x20
        0x00, 0x00, 0x00, 0x00,                          // seq 0
        0x1c, 0x00, 0x00, 0x00,                          // len 28
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // base_ns
        0x01, 0x00,                                      // count 1
        0x00, 0x00,                                      // reserved
        0x02, 0x00, 0x00, 0x00,                          // dt_us
        0x03, 0x00, 0x00, 0x00,                          // signal
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f,  // value 1.0
    };
    p2::LiveSamples msg;
    msg.baseNs = 1;
    msg.records = {{2u, 3u, 1.0}};
    CHECK(p2::encodeLiveSamples(msg) == expected);
    const auto back = p2::decodeLiveSamples(expected);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->records.size() == 1);
        CHECK(back->records[0].value == 1.0);
    }
}

void testHandBuiltTile() {
    // Level 0, qid 1, signal 2, sealed and last, index -1, offset 5, one sample
    // at t_ms 1.0 with value 2.0.
    const Bytes expected = {
        0x50, 0x32, 0x02, 0x22,                          // magic, version, type 0x22
        0x00, 0x00, 0x00, 0x00,                          // seq 0
        0x2c, 0x00, 0x00, 0x00,                          // len 44
        0x01, 0x00, 0x00, 0x00,                          // qid
        0x02, 0x00, 0x00, 0x00,                          // signal
        0x00,                                            // level 0
        0x03,                                            // flags: sealed, last
        0x00, 0x00,                                      // reserved
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // index -1
        0x05, 0x00, 0x00, 0x00,                          // offset
        0x01, 0x00, 0x00, 0x00,                          // n
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f,  // t_ms 1.0
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,  // value 2.0
    };
    p2::TilePiece piece;
    piece.qid = 1;
    piece.signal = 2;
    piece.level = 0;
    piece.flags = 3;
    piece.index = -1;
    piece.offset = 5;
    piece.tMs = {1.0};
    piece.value = {2.0};
    CHECK(p2::encodeTile(piece) == expected);
    const auto back = p2::decodeTile(expected);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->index == -1);
        CHECK(back->offset == 5);
        CHECK(back->value.size() == 1);
        CHECK(back->value[0] == 2.0);
    }
}

void testEveryPrefixRejected() {
    expectPrefixesRejected(p2::encodeLiveSamples(sampleLive()), [](Span b) { return p2::decodeLiveSamples(b).has_value(); });
    expectPrefixesRejected(p2::encodeGap(sampleGap()), [](Span b) { return p2::decodeGap(b).has_value(); });
    expectPrefixesRejected(p2::encodeTile(sampleTileLevel0()), [](Span b) { return p2::decodeTile(b).has_value(); });
    expectPrefixesRejected(p2::encodeTile(sampleTileLevel2()), [](Span b) { return p2::decodeTile(b).has_value(); });
    expectPrefixesRejected(p2::encodeVideoChunk(sampleVideo()), [](Span b) { return p2::decodeVideoChunk(b).has_value(); });
    expectPrefixesRejected(p2::encodeGap(sampleGap()), [](Span b) { return p2::decodeFrame(b).has_value(); });
}

void testWrongTypeRejected() {
    const Bytes live = p2::encodeLiveSamples(sampleLive());
    const Bytes gap = p2::encodeGap(sampleGap());
    const Bytes tile = p2::encodeTile(sampleTileLevel0());
    const Bytes video = p2::encodeVideoChunk(sampleVideo());

    CHECK(p2::decodeLiveSamples(live).has_value());
    CHECK(!p2::decodeLiveSamples(gap).has_value());
    CHECK(!p2::decodeLiveSamples(tile).has_value());
    CHECK(!p2::decodeLiveSamples(video).has_value());

    CHECK(!p2::decodeGap(live).has_value());
    CHECK(p2::decodeGap(gap).has_value());
    CHECK(!p2::decodeGap(tile).has_value());
    CHECK(!p2::decodeGap(video).has_value());

    CHECK(!p2::decodeTile(live).has_value());
    CHECK(!p2::decodeTile(gap).has_value());
    CHECK(p2::decodeTile(tile).has_value());
    CHECK(!p2::decodeTile(video).has_value());

    CHECK(!p2::decodeVideoChunk(live).has_value());
    CHECK(!p2::decodeVideoChunk(gap).has_value());
    CHECK(!p2::decodeVideoChunk(tile).has_value());
    CHECK(p2::decodeVideoChunk(video).has_value());
}

void testBadMagicVersionAndLength() {
    const Bytes good = p2::encodeGap(sampleGap());

    Bytes b = good;
    b[0] = 0x51;
    CHECK(!p2::decodeFrame(b).has_value());
    CHECK(!p2::decodeGap(b).has_value());

    b = good;
    b[1] = 0x33;
    CHECK(!p2::decodeFrame(b).has_value());

    b = good;
    b[2] = 1;
    CHECK(!p2::decodeFrame(b).has_value());
    CHECK(!p2::decodeGap(b).has_value());

    b = good;
    b[2] = 3;
    CHECK(!p2::decodeFrame(b).has_value());

    b = good;
    b[8] = 15;  // len 15 with 16 payload bytes
    CHECK(!p2::decodeFrame(b).has_value());
    CHECK(!p2::decodeGap(b).has_value());

    b = good;
    b[8] = 17;
    CHECK(!p2::decodeFrame(b).has_value());

    b = good;
    b.push_back(0);  // one byte past the header's len
    CHECK(!p2::decodeFrame(b).has_value());
    CHECK(!p2::decodeGap(b).has_value());

    // A len of 2^32 - 1 must not wrap the size check.
    const Bytes huge = {0x50, 0x32, 0x02, 0x21, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
    CHECK(!p2::decodeFrame(huge).has_value());
    CHECK(!p2::decodeGap(huge).has_value());
    CHECK(!p2::decodeFrame(Bytes{}).has_value());
}

void testLiveSamplesCountMismatch() {
    p2::LiveSamples msg;
    msg.baseNs = 5;
    msg.records = {{1u, 1u, 1.0}, {2u, 2u, 2.0}};
    const Bytes good = p2::encodeLiveSamples(msg);
    CHECK(p2::decodeLiveSamples(good).has_value());

    Bytes b = good;  // count is at offset 20
    b[20] = 3;
    CHECK(p2::decodeFrame(b).has_value());
    CHECK(!p2::decodeLiveSamples(b).has_value());

    b = good;
    b[20] = 1;
    CHECK(!p2::decodeLiveSamples(b).has_value());

    b = good;
    b[20] = 0;
    CHECK(!p2::decodeLiveSamples(b).has_value());
}

void testLiveSamplesReservedIgnored() {
    const Bytes good = p2::encodeLiveSamples(sampleLive());
    Bytes b = good;
    b[22] = 0xee;
    b[23] = 0xdd;
    const auto back = p2::decodeLiveSamples(b);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->records.size() == 3);
        CHECK(back->records[1].value == -1.5);
    }
}

void testTileNMismatch() {
    const Bytes good = p2::encodeTile(sampleTileLevel0());
    CHECK(p2::decodeTile(good).has_value());

    Bytes b = good;  // n is at offset 36
    b[36] = 4;
    CHECK(p2::decodeFrame(b).has_value());
    CHECK(!p2::decodeTile(b).has_value());

    b = good;
    b[36] = 2;
    CHECK(!p2::decodeTile(b).has_value());

    // A level-above-0 frame has four arrays; the same bytes as level 0 do not fit it.
    b = good;
    b[20] = 1;  // level
    CHECK(!p2::decodeTile(b).has_value());
}

void testTileAbove64KiBRejected() {
    p2::TilePiece piece;
    piece.level = 0;
    piece.tMs.assign(4096, 1.0);
    piece.value.assign(4096, 2.0);
    CHECK(!p2::decodeTile(p2::encodeTile(piece)).has_value());

    piece.tMs.resize(4090);
    piece.value.resize(4090);
    CHECK(p2::decodeTile(p2::encodeTile(piece)).has_value());
}

void testTileReservedIgnored() {
    Bytes b = p2::encodeTile(sampleTileLevel0());
    b[22] = 0xab;
    b[23] = 0xcd;
    const auto back = p2::decodeTile(b);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->tMs.size() == 3);
        CHECK(back->value[1] == -2.25);
    }
}

void testVideoChunkLimit() {
    p2::VideoChunk chunk;
    chunk.payload.assign(p2::kVideoChunkMax + 1, std::uint8_t{0xab});
    CHECK(!p2::decodeVideoChunk(p2::encodeVideoChunk(chunk)).has_value());

    chunk.payload.assign(p2::kVideoChunkMax, std::uint8_t{0xab});
    const auto back = p2::decodeVideoChunk(p2::encodeVideoChunk(chunk));
    CHECK(back.has_value());
    if (back) {
        CHECK(back->payload.size() == p2::kVideoChunkMax);
    }
}

}  // namespace

void runProtocolV2Tests() {
    testHeaderFields();
    testLiveSamplesRoundTrip();
    testLiveSamplesEmptyAndBaseExtremes();
    testGapRoundTrip();
    testTileLevel0RoundTrip();
    testTileLevelAboveZeroRoundTrip();
    testTileIndexExtremes();
    testTileShortArraysArePadded();
    testVideoChunkRoundTrip();
    testHandBuiltGap();
    testHandBuiltLiveSamples();
    testHandBuiltTile();
    testEveryPrefixRejected();
    testWrongTypeRejected();
    testBadMagicVersionAndLength();
    testLiveSamplesCountMismatch();
    testLiveSamplesReservedIgnored();
    testTileNMismatch();
    testTileAbove64KiBRejected();
    testTileReservedIgnored();
    testVideoChunkLimit();
}
