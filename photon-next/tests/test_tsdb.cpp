#include "wire/canp.hpp"
#include "decode/dbc.hpp"
#include "base/endian.hpp"
#include "wire/protocol.hpp"
#include "store/tsdb.hpp"
#include "store/vsd1.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

void runTsdbTests();

namespace {

namespace fs = std::filesystem;
namespace tsdb = photon::tsdb;

constexpr const char* kFixture = R"(
VERSION ""

BO_ 256 EngineData: 8 Vector
 SG_ RPM : 0|16@1+ (0.25,0) [0|8000] "rpm" Vector
 SG_ Temp : 16|8@1- (1,-40) [-40|215] "C" Vector

BO_ 512 Status: 8 Vector
 SG_ Flags : 0|8@1+ (1,0) [0|255] "" Vector
)";

fs::path uniqueDir(const char* tag) {
    const auto base = fs::temp_directory_path() / "photon-tsdb-tests";
    fs::create_directories(base);
    static int n = 0;
    return base / (std::string(tag) + "-" + std::to_string(++n));
}

std::vector<std::uint8_t> encodeVsd1(std::uint64_t id, const photon::canp::Batch& batch) {
    const auto payload = photon::canp::encodeBatch(batch);
    std::vector<std::uint8_t> out;
    photon::writeU32Be(out, photon::vsd1::kMagic);
    photon::writeU32Be(out, static_cast<std::uint32_t>(payload.size()));
    photon::writeU64Be(out, id);
    photon::writeU32Be(out, 0);
    photon::writeU32Be(out, 0);
    photon::writeBytes(out, payload);
    return out;
}

void writeLog(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void testPageCodec() {
    tsdb::PageHeader hdr{};
    std::vector<std::uint64_t> t;
    std::vector<std::int64_t> v;

    const auto empty = tsdb::encodeRawPage(7, {}, {}, true);
    CHECK(empty.size() == tsdb::kPageHeaderBytes);
    CHECK(tsdb::decodeRawPage(empty, hdr, t, v));
    CHECK(hdr.magic == tsdb::kPageMagic);
    CHECK(hdr.signalId == 7);
    CHECK(hdr.sampleCount == 0);
    CHECK(t.empty());
    CHECK(v.empty());
    CHECK((hdr.flags & tsdb::kFlagLz4) == 0);

    const std::uint64_t t1[] = {1000};
    const std::int64_t v1[] = {42};
    const auto single = tsdb::encodeRawPage(1, t1, v1, true);
    CHECK(tsdb::decodeRawPage(single, hdr, t, v));
    CHECK(hdr.sampleCount == 1);
    CHECK(t.size() == 1 && t[0] == 1000);
    CHECK(v.size() == 1 && v[0] == 42);

    const std::uint64_t tConst[] = {10, 20, 30, 40};
    const std::int64_t vConst[] = {5, 5, 5, 5};
    const auto constant = tsdb::encodeRawPage(2, tConst, vConst, false);
    CHECK(tsdb::decodeRawPage(constant, hdr, t, v));
    CHECK(hdr.vEnc == tsdb::kVEncConstant);
    CHECK(hdr.tEnc == tsdb::kTEncStride);
    CHECK(t.size() == 4 && t[3] == 40);
    CHECK(v[0] == 5 && v[3] == 5);
    CHECK((hdr.flags & tsdb::kFlagLastRun) == 0);

    std::vector<std::uint64_t> tVar;
    std::vector<std::int64_t> vVar;
    std::uint64_t cur = 1'000'000;
    for (int i = 0; i < 64; ++i) {
        cur += static_cast<std::uint64_t>(10 + (i % 3));
        tVar.push_back(cur);
        vVar.push_back(100 - i * 3);
    }
    const auto varied = tsdb::encodeRawPage(9, tVar, vVar, true);
    CHECK(tsdb::decodeRawPage(varied, hdr, t, v));
    CHECK(hdr.tEnc == tsdb::kTEncDelta);
    CHECK(hdr.vEnc == tsdb::kVEncZigzag);
    CHECK(t == tVar);
    CHECK(v == vVar);

    std::vector<tsdb::RollupBucket> slots;
    tsdb::RollupBucket a{};
    a.startMs = 0;
    a.min = -4;
    a.max = 10;
    a.first = 1;
    a.last = 2;
    a.sum = 9;
    a.count = 3;
    a.present = true;
    tsdb::RollupBucket hole{};
    hole.startMs = 100;
    hole.present = false;
    tsdb::RollupBucket b = a;
    b.startMs = 200;
    b.min = 0;
    b.max = 1;
    b.first = 0;
    b.last = 1;
    b.sum = 1;
    b.count = 1;
    slots.push_back(a);
    slots.push_back(hole);
    slots.push_back(b);
    const auto roll = tsdb::encodeRollupPage(3, 1, 0, 100, slots, true);
    std::vector<tsdb::RollupBucket> back;
    CHECK(tsdb::decodeRollupPage(roll, hdr, 100, back));
    CHECK(hdr.level == 1);
    CHECK(hdr.tEnc == tsdb::kTEncImplicit);
    CHECK((hdr.flags & tsdb::kFlagDense) == 0);
    CHECK(back.size() == 3);
    CHECK(back[0].present && back[0].min == -4 && back[0].sum == 9 && back[0].count == 3);
    CHECK(!back[1].present);
    CHECK(back[2].present && back[2].first == 0 && back[2].last == 1);

    std::vector<tsdb::RollupBucket> dense = {a, a, a};
    dense[1].startMs = 100;
    dense[2].startMs = 200;
    const auto densePage = tsdb::encodeRollupPage(3, 2, 0, 1000, dense, true);
    CHECK(tsdb::decodeRollupPage(densePage, hdr, 1000, back));
    CHECK((hdr.flags & tsdb::kFlagDense) != 0);
    CHECK(back.size() == 3);
    CHECK(back[2].startMs == 2000);
}

void testIndex() {
    tsdb::IndexHeader hdr;
    hdr.level = 0;
    hdr.minMs = 100;
    hdr.maxMs = 900;
    hdr.bucketMs = 0;
    hdr.signalCount = 2;
    std::vector<tsdb::IndexRecord> recs(3);
    recs[0].signalId = 1;
    recs[0].startMs = 100;
    recs[0].endMs = 200;
    recs[0].fileNo = 0;
    recs[0].offset = 0;
    recs[0].pageBytes = 32;
    recs[0].sampleCount = 4;
    recs[0].vMin = 1.0f;
    recs[0].vMax = 2.0f;
    recs[1].signalId = 1;
    recs[1].startMs = 400;
    recs[1].endMs = 500;
    recs[1].sampleCount = 2;
    recs[2].signalId = 9;
    recs[2].startMs = 10;
    recs[2].endMs = 900;
    const auto bytes = tsdb::encodeIndex(hdr, recs);
    CHECK(bytes.size() == tsdb::kIndexHeaderBytes + 3 * tsdb::kIndexRecordBytes);
    tsdb::IndexHeader back{};
    std::vector<tsdb::IndexRecord> out;
    CHECK(tsdb::decodeIndex(bytes, back, out));
    CHECK(back.magic == tsdb::kIndexMagic);
    CHECK(back.recordCount == 3);
    CHECK(back.minMs == 100);
    CHECK(out[0].signalId == 1 && out[0].vMax == 2.0f);
    CHECK(out[2].signalId == 9);

    CHECK(tsdb::indexLowerBound(out, 1, 0) == 0);
    CHECK(tsdb::indexLowerBound(out, 1, 100) == 0);
    CHECK(tsdb::indexLowerBound(out, 1, 101) == 1);
    CHECK(tsdb::indexLowerBound(out, 1, 400) == 1);
    CHECK(tsdb::indexLowerBound(out, 1, 401) == 2);
    CHECK(tsdb::indexLowerBound(out, 9, 10) == 2);
    CHECK(tsdb::indexLowerBound(out, 10, 0) == 3);
}

void testTileAndLevel() {
    CHECK(tsdb::tileSpanMs(0) == 60'000);
    CHECK(tsdb::tileSpanMs(1) == 100 * 1024);
    CHECK(tsdb::tileSpanMs(2) == 1000 * 1024);
    CHECK(tsdb::tileSpanMs(3) == 10'000 * 1024);
    CHECK(tsdb::tileSpanMs(4) == 60'000 * 1024);
    CHECK(tsdb::tileSpanMs(5) == 600'000 * 1024);

    const std::uint64_t tFast = 1'784'841'605'123ull;
    const std::uint64_t tSlow = tFast + 3;
    CHECK(tsdb::tileIndexOf(tFast, 0) == tsdb::tileIndexOf(tSlow, 0));
    CHECK(tsdb::tileIndexOf(tFast, 2) == tsdb::tileIndexOf(tSlow, 2));
    CHECK(tsdb::tileIndexOf(0, 0) == 0);
    CHECK(tsdb::tileIndexOf(59'999, 0) == 0);
    CHECK(tsdb::tileIndexOf(60'000, 0) == 1);

    CHECK(tsdb::shouldBuildLevel(57.0, 1));
    CHECK(!tsdb::shouldBuildLevel(0.3, 1));
    CHECK(!tsdb::shouldBuildLevel(0.3, 2));
    CHECK(tsdb::shouldBuildLevel(0.3, 4));
    CHECK(tsdb::shouldBuildLevel(5.0, 2));

    const auto l2s = tsdb::pickLevel(0, 2000, 2000, 10'000, 0x3f);
    const auto l7200s = tsdb::pickLevel(0, 7'200'000, 2000, 10'000'000, 0x3f);
    CHECK(l2s <= 1);
    CHECK(l7200s >= 2);
    const auto buckets2 = 2000u / std::max(1u, tsdb::bucketMsOf(l2s));
    const auto buckets7200 = 7'200'000u / std::max(1u, tsdb::bucketMsOf(l7200s));
    CHECK(buckets2 <= 4000);
    CHECK(buckets7200 <= 4000);
    CHECK(l7200s == tsdb::pickLevel(0, 7'200'000, 2000, 10'000'000, 0x3f));

    const auto rawFits = tsdb::pickLevel(0, 7'200'000, 2000, 100, 0x3f);
    CHECK(rawFits == 0);
}

void testStoreRoundTrip() {
    const auto dir = uniqueDir("store");
    tsdb::SessionManifest man;
    man.id = "test-store";
    tsdb::Store w;
    std::string error;
    CHECK(w.create(dir, man, &error));
    tsdb::SignalInfo info;
    info.id = 42;
    info.name = "RPM";
    info.factor = 0.25;
    w.setSignal(info);
    for (int i = 0; i < 5000; ++i) {
        w.append(42, 10'000 + static_cast<std::uint64_t>(i) * 10, 1000 + i);
    }
    CHECK(w.finish(&error));
    w.close();

    tsdb::Store r;
    CHECK(r.open(dir, &error));
    CHECK(r.manifest().samples == 5000);
    CHECK(!r.index(0).empty());
    tsdb::Query q;
    q.fromMs = 10'000;
    q.toMs = 10'000 + 4999 * 10;
    q.signalIds = {42};
    q.maxPoints = 10000;
    const auto result = r.query(q);
    CHECK(result.series.size() == 1);
    CHECK(result.series[0].level == 0);
    CHECK(result.series[0].timestamps.size() == 5000);
    CHECK(result.series[0].raw.front() == 1000);
    CHECK(result.series[0].raw.back() == 1000 + 4999);

    const auto mid = r.query(tsdb::Query{10'500, 10'700, {42}, 2000});
    CHECK(mid.series[0].timestamps.size() == 21);
    CHECK(mid.series[0].timestamps.front() == 10'500);
    CHECK(mid.series[0].timestamps.back() == 10'700);
}

void testBoundarySamples() {
    const auto dir = uniqueDir("sparse");
    tsdb::SessionManifest man;
    man.id = "sparse";
    tsdb::Store w;
    std::string error;
    CHECK(w.create(dir, man, &error));
    const std::uint32_t sid = photon::dbc::signalIdOf("Status", "Flags");
    const std::uint64_t t0 = 1'000'000;
    w.append(sid, t0, 3);
    w.append(sid, t0 + 160'000, 7);
    w.append(sid, t0 + 320'000, 1);
    CHECK(w.finish(&error));
    w.close();

    tsdb::Store r;
    CHECK(r.open(dir, &error));
    const std::uint64_t from = t0 + 80'000;
    const std::uint64_t to = from + 5'000;
    const auto result = r.query(tsdb::Query{from, to, {sid}, 2000});
    CHECK(result.series.size() == 1);
    CHECK(result.series[0].timestamps.empty());
    CHECK(result.series[0].prior.has_value());
    CHECK(result.series[0].prior->timestampMs == t0);
    CHECK(result.series[0].prior->raw == 3);
    CHECK(result.series[0].next.has_value());
    CHECK(result.series[0].next->timestampMs == t0 + 160'000);
    CHECK(result.series[0].next->raw == 7);

    const auto exact = r.prior(sid, t0);
    CHECK(exact.has_value());
    CHECK(exact->raw == 3);
    const auto after = r.next(sid, t0 + 160'000);
    CHECK(after.has_value());
    CHECK(after->raw == 7);
}

void testRangeBoundsAndLevels() {
    const auto dir = uniqueDir("bounds");
    tsdb::SessionManifest man;
    man.id = "bounds";
    tsdb::Store w;
    std::string error;
    CHECK(w.create(dir, man, &error));
    tsdb::SignalInfo fast;
    fast.id = 1;
    fast.name = "Fast";
    fast.factor = 1.0;
    w.setSignal(fast);
    tsdb::SignalInfo slow;
    slow.id = 2;
    slow.name = "Slow";
    w.setSignal(slow);
    for (int i = 0; i < 8000; ++i) {
        w.append(1, 10'000 + static_cast<std::uint64_t>(i) * 10, i);
    }
    w.append(2, 10'000, 7);
    w.append(2, 10'000 + 160'000, 9);
    CHECK(w.finish(&error));
    w.close();

    tsdb::Store r;
    CHECK(r.open(dir, &error));
    CHECK((r.levelsOf(1) & 0x02) != 0);
    CHECK((r.levelsOf(2) & 0x02) == 0);

    const auto mid = r.query(tsdb::Query{10'500, 10'700, {1}, 2000});
    CHECK(mid.series[0].level == 0);
    CHECK(mid.series[0].timestamps.size() == 21);

    const auto mixed = r.query(tsdb::Query{10'000, 10'000 + 7999 * 10, {1, 2}, 2000});
    CHECK(mixed.series.size() == 2);
    CHECK(mixed.series[0].level >= 1);
    CHECK(mixed.series[1].level == 0);

    const auto roll = r.query(tsdb::Query{10'100, 10'250, {1}, 4});
    CHECK(roll.series[0].level >= 1);
    CHECK(roll.series[0].bucketMs >= 100);
    bool sawAtFrom = false;
    for (const auto& b : roll.series[0].buckets) {
        if (!b.present || b.count == 0) {
            continue;
        }
        if (b.startMs + roll.series[0].bucketMs == 10'100) {
            sawAtFrom = true;
        }
    }
    CHECK(!sawAtFrom);
}

void testFoldKeepsGaps() {
    const auto dir = uniqueDir("foldgap");
    tsdb::SessionManifest man;
    man.id = "foldgap";
    tsdb::Store w;
    std::string error;
    CHECK(w.create(dir, man, &error));
    const std::uint32_t sid = 99;
    for (int i = 0; i < 40; ++i) {
        w.append(sid, static_cast<std::uint64_t>(i) * 10, 1);
    }
    for (int i = 0; i < 40; ++i) {
        w.append(sid, 10'000 + static_cast<std::uint64_t>(i) * 10, 50);
    }
    CHECK(w.finish(&error));
    w.close();

    tsdb::Store r;
    CHECK(r.open(dir, &error));
    // 80 raw samples, budget = 16 → not L0. Span 11000 / L2 1000ms = 11 ≤ 16 → L2.
    // 12 source slots fold once to 6 buckets at 2000 ms. The 390–10000 hole
    // must remain empty slots, not get absorbed into a present bucket.
    const auto res = r.query(tsdb::Query{0, 11'000, {sid}, 8});
    CHECK(res.series[0].level == 2);
    CHECK(res.series[0].bucketMs == 2000);
    CHECK(res.series[0].buckets.size() == 6);
    const std::uint64_t starts[] = {0, 2000, 4000, 6000, 8000, 10000};
    const bool present[] = {true, false, false, false, false, true};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(res.series[0].buckets[i].startMs == starts[i]);
        CHECK(res.series[0].buckets[i].present == present[i]);
        if (present[i]) {
            CHECK(res.series[0].buckets[i].count > 0);
        } else {
            CHECK(res.series[0].buckets[i].count == 0);
        }
    }
}

void testUnsignedRollupMinMaxMean() {
    const auto dir = uniqueDir("u64roll");
    tsdb::SessionManifest man;
    man.id = "u64roll";
    tsdb::Store w;
    std::string error;
    CHECK(w.create(dir, man, &error));
    tsdb::SignalInfo info;
    info.id = 7;
    info.name = "Wide";
    info.bits = 64;
    info.isSigned = false;
    info.factor = 1.0;
    w.setSignal(info);
    const std::int64_t huge = static_cast<std::int64_t>(1ull << 63);
    for (int i = 0; i < 4; ++i) {
        w.append(7, static_cast<std::uint64_t>(i) * 10, huge);
    }
    CHECK(w.finish(&error));
    w.close();

    tsdb::Store r;
    CHECK(r.open(dir, &error));
    const auto res = r.query(tsdb::Query{0, 40, {7}, 1});
    CHECK(!res.series.empty());
    CHECK(res.series[0].level >= 1);
    CHECK(!res.series[0].buckets.empty());
    bool saw = false;
    for (const auto& b : res.series[0].buckets) {
        if (!b.present || b.count == 0) {
            continue;
        }
        saw = true;
        CHECK(static_cast<std::uint64_t>(b.min) == (1ull << 63));
        CHECK(static_cast<std::uint64_t>(b.max) == (1ull << 63));
        CHECK(b.count == 4);
        CHECK(b.sumHi == 2);
        CHECK(static_cast<std::uint64_t>(b.sum) == 0);
        const double mean = std::ldexp(static_cast<double>(b.sumHi), 64) / static_cast<double>(b.count);
        CHECK(mean == 9223372036854775808.0);
        CHECK(static_cast<double>(static_cast<std::uint64_t>(b.sum)) / static_cast<double>(b.count) == 0.0);
    }
    CHECK(saw);

    std::vector<tsdb::RollupBucket> slots(1);
    slots[0].startMs = 0;
    slots[0].min = huge;
    slots[0].max = huge;
    slots[0].first = huge;
    slots[0].last = huge;
    slots[0].sum = 0;
    slots[0].sumHi = 1;
    slots[0].count = 2;
    slots[0].present = true;
    const auto widePage = tsdb::encodeRollupPage(7, 1, 0, 100, slots, true);
    tsdb::PageHeader hdr{};
    std::vector<tsdb::RollupBucket> back;
    CHECK(tsdb::decodeRollupPage(widePage, hdr, 100, back));
    CHECK(hdr.magic == tsdb::kPageMagicV2);
    CHECK((hdr.flags & tsdb::kFlagWideSum) != 0);
    CHECK(back.size() == 1);
    CHECK(back[0].sumHi == 1);
    CHECK(static_cast<std::uint64_t>(back[0].sum) == 0);

    slots[0].sumHi = 0;
    slots[0].sum = 42;
    slots[0].count = 1;
    auto v1page = tsdb::encodeRollupPage(7, 1, 0, 100, slots, true);
    CHECK(v1page.size() >= 4);
    v1page[0] = static_cast<std::uint8_t>(tsdb::kPageMagic & 0xff);
    v1page[1] = static_cast<std::uint8_t>((tsdb::kPageMagic >> 8) & 0xff);
    v1page[2] = static_cast<std::uint8_t>((tsdb::kPageMagic >> 16) & 0xff);
    v1page[3] = static_cast<std::uint8_t>((tsdb::kPageMagic >> 24) & 0xff);
    v1page[23] = static_cast<std::uint8_t>(v1page[23] & ~tsdb::kFlagWideSum);
    CHECK(tsdb::decodeRollupPage(v1page, hdr, 100, back));
    CHECK(hdr.magic == tsdb::kPageMagic);
    CHECK((hdr.flags & tsdb::kFlagWideSum) == 0);
    CHECK(back[0].sum == 42);
    CHECK(back[0].sumHi == 0);

    {
        tsdb::SessionManifest man;
        man.id = "u64bound";
        const auto boundDir = uniqueDir("u64bound");
        tsdb::Store w;
        CHECK(w.create(boundDir, man, &error));
        info.name = "UBound";
        w.setSignal(info);
        w.append(7, 0, static_cast<std::int64_t>(~0ull));
        w.append(7, 10, 0);
        w.append(7, 20, 0);
        CHECK(w.finish(&error));
        w.close();
        tsdb::Store r;
        CHECK(r.open(boundDir, &error));
        const auto one = r.query(tsdb::Query{0, 30, {7}, 1});
        CHECK(!one.series.empty());
        bool sawOne = false;
        for (const auto& b : one.series[0].buckets) {
            if (!b.present || b.count == 0) {
                continue;
            }
            sawOne = true;
            CHECK(!b.wide);
            CHECK(b.sumHi == 0);
            CHECK(static_cast<std::uint64_t>(b.sum) == ~0ull);
        }
        CHECK(sawOne);
    }
    {
        tsdb::SessionManifest man;
        man.id = "u64over";
        const auto overDir = uniqueDir("u64over");
        tsdb::Store w;
        CHECK(w.create(overDir, man, &error));
        info.name = "UOver";
        w.setSignal(info);
        w.append(7, 0, static_cast<std::int64_t>(~0ull));
        w.append(7, 10, 1);
        w.append(7, 20, 0);
        CHECK(w.finish(&error));
        w.close();
        tsdb::Store r;
        CHECK(r.open(overDir, &error));
        const auto two = r.query(tsdb::Query{0, 30, {7}, 1});
        CHECK(!two.series.empty());
        bool sawTwo = false;
        for (const auto& b : two.series[0].buckets) {
            if (!b.present || b.count == 0) {
                continue;
            }
            sawTwo = true;
            CHECK(b.wide);
            CHECK(b.sumHi == 1);
            CHECK(static_cast<std::uint64_t>(b.sum) == 0);
        }
        CHECK(sawTwo);
    }
}

void testSignedRollupWideSum() {
    const auto dir = uniqueDir("i64roll");
    tsdb::SessionManifest man;
    man.id = "i64roll";
    tsdb::Store w;
    std::string error;
    CHECK(w.create(dir, man, &error));
    tsdb::SignalInfo info;
    info.id = 8;
    info.name = "SWide";
    info.bits = 64;
    info.isSigned = true;
    info.factor = 1.0;
    w.setSignal(info);
    w.append(8, 0, INT64_MAX);
    w.append(8, 10, 1);
    w.append(8, 20, INT64_MAX);
    w.append(8, 30, 1);
    CHECK(w.finish(&error));
    w.close();

    tsdb::Store r;
    CHECK(r.open(dir, &error));
    const auto res = r.query(tsdb::Query{0, 40, {8}, 1});
    CHECK(!res.series.empty());
    CHECK(res.series[0].level >= 1);
    bool saw = false;
    for (const auto& b : res.series[0].buckets) {
        if (!b.present || b.count == 0) {
            continue;
        }
        saw = true;
        CHECK(b.count == 4);
        CHECK(b.wide);
        CHECK(b.min == 1);
        CHECK(b.max == INT64_MAX);
        CHECK(b.sumHi == 1);
        CHECK(static_cast<std::uint64_t>(b.sum) == 0);
        const double mean = std::ldexp(static_cast<double>(b.sumHi), 64) / static_cast<double>(b.count);
        CHECK(mean == 4611686018427387904.0);
    }
    CHECK(saw);

    {
        tsdb::SessionManifest oneMan;
        oneMan.id = "i64one";
        const auto oneDir = uniqueDir("i64one");
        tsdb::Store one;
        CHECK(one.create(oneDir, oneMan, &error));
        one.setSignal(info);
        one.append(8, 0, INT64_MAX);
        one.append(8, 10, 0);
        one.append(8, 20, 0);
        CHECK(one.finish(&error));
        one.close();
        tsdb::Store ro;
        CHECK(ro.open(oneDir, &error));
        const auto oneRes = ro.query(tsdb::Query{0, 30, {8}, 1});
        bool sawOne = false;
        for (const auto& b : oneRes.series[0].buckets) {
            if (!b.present || b.count == 0) {
                continue;
            }
            sawOne = true;
            CHECK(!b.wide);
            CHECK(b.sum == INT64_MAX);
            CHECK(b.sumHi == 0);
        }
        CHECK(sawOne);
    }

    {
        tsdb::SessionManifest negMan;
        negMan.id = "i64neg";
        const auto negDir = uniqueDir("i64neg");
        tsdb::Store nw;
        CHECK(nw.create(negDir, negMan, &error));
        nw.setSignal(info);
        nw.append(8, 0, INT64_MIN);
        nw.append(8, 10, 0);
        nw.append(8, 20, 0);
        CHECK(nw.finish(&error));
        nw.close();
        tsdb::Store nr;
        CHECK(nr.open(negDir, &error));
        const auto one = nr.query(tsdb::Query{0, 30, {8}, 1});
        bool sawOne = false;
        for (const auto& b : one.series[0].buckets) {
            if (!b.present || b.count == 0) {
                continue;
            }
            sawOne = true;
            CHECK(!b.wide);
            CHECK(b.sum == INT64_MIN);
        }
        CHECK(sawOne);
    }
    {
        tsdb::SessionManifest negMan;
        negMan.id = "i64neg2";
        const auto negDir = uniqueDir("i64neg2");
        tsdb::Store nw;
        CHECK(nw.create(negDir, negMan, &error));
        nw.setSignal(info);
        nw.append(8, 0, INT64_MIN);
        nw.append(8, 10, INT64_MIN);
        nw.append(8, 20, 0);
        CHECK(nw.finish(&error));
        nw.close();
        tsdb::Store nr;
        CHECK(nr.open(negDir, &error));
        const auto two = nr.query(tsdb::Query{0, 30, {8}, 1});
        bool sawTwo = false;
        for (const auto& b : two.series[0].buckets) {
            if (!b.present || b.count == 0) {
                continue;
            }
            sawTwo = true;
            CHECK(b.wide);
            CHECK(b.count == 3);
            CHECK(b.sumHi == ~0ull);
            CHECK(static_cast<std::uint64_t>(b.sum) == 0);
        }
        CHECK(sawTwo);
    }
}

void testEncodedFlushThreshold() {
    std::string error;
    {
        const auto dir = uniqueDir("constpage");
        tsdb::SessionManifest man;
        man.id = "constpage";
        tsdb::Store w;
        CHECK(w.create(dir, man, &error));
        tsdb::SignalInfo info;
        info.id = 3;
        info.name = "Const";
        info.isSigned = true;
        w.setSignal(info);
        for (int i = 0; i < 4000; ++i) {
            w.append(3, static_cast<std::uint64_t>(i) * 10, 7);
        }
        CHECK(w.finish(&error));
        w.close();
        tsdb::Store r;
        CHECK(r.open(dir, &error));
        const auto& idx = r.index(0);
        CHECK(idx.size() == 1);
        CHECK(idx[0].sampleCount == 4000);
        CHECK(idx[0].pageBytes < 128);
    }
    {
        const auto dir = uniqueDir("noisypage");
        tsdb::SessionManifest man;
        man.id = "noisypage";
        tsdb::Store w;
        CHECK(w.create(dir, man, &error));
        tsdb::SignalInfo info;
        info.id = 4;
        info.name = "Noisy";
        info.isSigned = true;
        w.setSignal(info);
        std::uint64_t x = 0x9e3779b97f4a7c15ull;
        for (int i = 0; i < 20000; ++i) {
            x = x * 6364136223846793005ull + 1;
            w.append(4, static_cast<std::uint64_t>(i), static_cast<std::int64_t>(x));
        }
        CHECK(w.finish(&error));
        w.close();
        tsdb::Store r;
        CHECK(r.open(dir, &error));
        const auto& idx = r.index(0);
        CHECK(idx.size() >= 2);
        for (const auto& rec : idx) {
            CHECK(rec.pageBytes <= tsdb::kPageHeaderBytes + tsdb::kPagePayloadCap + 32);
        }
        std::uint64_t samples = 0;
        for (const auto& rec : idx) {
            samples += rec.sampleCount;
        }
        CHECK(samples == 20000);
    }
}

void testCodecWrap() {
    tsdb::PageHeader hdr{};
    std::vector<std::uint64_t> t;
    std::vector<std::int64_t> v;
    const std::int64_t nearMax = 9223372036854775800ll;
    const std::uint64_t times[] = {10, 20, 30};
    const std::int64_t vals[] = {nearMax, nearMax - 5, nearMax};
    const auto page = tsdb::encodeRawPage(1, times, vals, true);
    CHECK(tsdb::decodeRawPage(page, hdr, t, v));
    CHECK(v.size() == 3);
    CHECK(v[0] == nearMax);
    CHECK(v[1] == nearMax - 5);
    CHECK(v[2] == nearMax);
}

void testIndexValidate() {
    auto db = photon::dbc::parse(kFixture);
    CHECK(db.has_value());
    const auto rpmId = photon::dbc::signalIdOf("EngineData", "RPM");
    const auto flagsId = photon::dbc::signalIdOf("Status", "Flags");

    std::vector<std::uint8_t> blob;
    std::uint64_t id = 1;
    for (int i = 0; i < 80; ++i) {
        photon::dbc::EncodedFrame engine{};
        CHECK(photon::dbc::encodeFrame(*db, "EngineData",
                                       std::vector<photon::dbc::SignalValue>{{"RPM", 250.0 + i}, {"Temp", 20.0}},
                                       engine));
        photon::canp::Batch batch{};
        batch.seq = static_cast<std::uint32_t>(i + 1);
        batch.timestamp = 2'000'000 + static_cast<std::uint64_t>(i) * 20;
        batch.count = 1;
        batch.packets[0].canId = engine.canId;
        batch.packets[0].dlc = engine.dlc;
        std::memcpy(batch.packets[0].data, engine.data, 8);
        auto frame = encodeVsd1(id++, batch);
        blob.insert(blob.end(), frame.begin(), frame.end());
        if (i % 8 == 0) {
            photon::dbc::EncodedFrame st{};
            CHECK(photon::dbc::encodeFrame(*db, "Status",
                                           std::vector<photon::dbc::SignalValue>{{"Flags", static_cast<double>(i)}},
                                           st));
            photon::canp::Batch sb{};
            sb.seq = 1000 + static_cast<std::uint32_t>(i);
            sb.timestamp = batch.timestamp;
            sb.count = 1;
            sb.packets[0].canId = st.canId;
            sb.packets[0].dlc = st.dlc;
            std::memcpy(sb.packets[0].data, st.data, 8);
            auto sf = encodeVsd1(id++, sb);
            blob.insert(blob.end(), sf.begin(), sf.end());
        }
    }
    const auto logs = uniqueDir("vsd1");
    writeLog(logs / "run.log", blob);
    const auto out = uniqueDir("indexed");
    std::string error;
    const auto stats = tsdb::indexLogs(logs, out, *db, "unit-1", &error);
    CHECK(error.empty());
    CHECK(stats.samples > 0);
    CHECK(stats.storeBytes > 0);
    CHECK(stats.sessionId == "unit-1");
    std::fprintf(stdout,
                 "tsdb measure session=%s source=%llu B store=%llu B samples=%llu frames=%llu elapsed=%.3f s\n",
                 stats.sessionId.c_str(), static_cast<unsigned long long>(stats.sourceBytes),
                 static_cast<unsigned long long>(stats.storeBytes), static_cast<unsigned long long>(stats.samples),
                 static_cast<unsigned long long>(stats.frames), stats.elapsedS);

    tsdb::Store store;
    CHECK(store.open(out / "unit-1", &error));
    const auto tQuery0 = std::chrono::steady_clock::now();
    const auto wide = store.query(tsdb::Query{store.manifest().minMs, store.manifest().maxMs, {rpmId, flagsId}, 2000});
    const auto tQuery1 = std::chrono::steady_clock::now();
    const double queryMs = std::chrono::duration<double, std::milli>(tQuery1 - tQuery0).count();
    std::fprintf(stdout, "tsdb query 2-signal full-range level=%d series=%zu latency=%.3f ms\n", wide.level,
                 wide.series.size(), queryMs);
    CHECK(tsdb::validateAgainstDecode(store, logs, *db, &error));
    CHECK(error.empty());

    std::vector<std::uint32_t> ids;
    std::vector<std::uint64_t> times;
    std::vector<std::int64_t> raws;
    CHECK(tsdb::collectDecodeTriples(logs, *db, ids, times, raws, &error));
    CHECK(!ids.empty());
    bool sawRpm = false;
    bool sawFlags = false;
    for (const auto idv : ids) {
        sawRpm = sawRpm || idv == rpmId;
        sawFlags = sawFlags || idv == flagsId;
    }
    CHECK(sawRpm);
    CHECK(sawFlags);

    const auto chunk = photon::proto::HistoryChunk{};
    (void)chunk;
}

void testHistoryChunk() {
    photon::proto::HistoryChunk chunk;
    chunk.queryId = 17;
    chunk.signalId = 42;
    chunk.level = 2;
    chunk.fields = photon::proto::kHistMin | photon::proto::kHistMax | photon::proto::kHistMean;
    chunk.flags = photon::proto::kHistFinal | photon::proto::kHistDense;
    chunk.chunkSeq = 3;
    chunk.baseMs = 1000;
    chunk.bucketMs = 1000;
    chunk.slotCount = 2;
    chunk.valueCount = 2;
    chunk.minV = {1.0f, 2.0f};
    chunk.maxV = {3.0f, 4.0f};
    chunk.meanV = {2.0f, 3.0f};
    const auto payload = photon::proto::encodeHistoryChunk(chunk);
    CHECK(payload.size() == 36 + 3 * 2 * 4);
    const auto back = photon::proto::decodeHistoryChunk(payload);
    CHECK(back.has_value());
    CHECK(back->queryId == 17);
    CHECK(back->signalId == 42);
    CHECK(back->level == 2);
    CHECK(back->chunkSeq == 3);
    CHECK(back->valueCount == 2);
    CHECK(back->slotCount == 2);
    CHECK(back->bucketMs == 1000);
    CHECK(back->minV.size() == 2 && back->minV[1] == 2.0f);
    CHECK(back->meanV[0] == 2.0f);
    CHECK((back->flags & photon::proto::kHistFinal) != 0);
    CHECK((back->flags & photon::proto::kHistDense) != 0);

    photon::proto::HistoryChunk sparse;
    sparse.queryId = 8;
    sparse.signalId = 5;
    sparse.level = 1;
    sparse.fields = photon::proto::kHistMin | photon::proto::kHistMax;
    sparse.chunkSeq = 0;
    sparse.baseMs = 0;
    sparse.bucketMs = 100;
    sparse.slotCount = 3;
    sparse.valueCount = 2;
    sparse.presence = {0x05};  // slots 0 and 2
    sparse.minV = {1.0f, 9.0f};
    sparse.maxV = {2.0f, 10.0f};
    const auto sparsePayload = photon::proto::encodeHistoryChunk(sparse);
    CHECK(sparsePayload.size() == 36 + 1 + 2 * 2 * 4);
    const auto sparseBack = photon::proto::decodeHistoryChunk(sparsePayload);
    CHECK(sparseBack.has_value());
    CHECK(sparseBack->slotCount == 3);
    CHECK(sparseBack->valueCount == 2);
    CHECK((sparseBack->flags & photon::proto::kHistDense) == 0);
    CHECK(sparseBack->presence.size() == 1);
    CHECK((sparseBack->presence[0] & 0x01) != 0);
    CHECK((sparseBack->presence[0] & 0x02) == 0);
    CHECK((sparseBack->presence[0] & 0x04) != 0);
    CHECK(sparseBack->minV[1] == 9.0f);

    photon::proto::HistoryChunk raw;
    raw.queryId = 1;
    raw.signalId = 9;
    raw.fields = photon::proto::kHistRaw;
    raw.bucketMs = 0;
    raw.timestamps = {10.0, 20.5};
    raw.rawV = {1.25f, -3.5f};
    const auto rawPayload = photon::proto::encodeHistoryChunk(raw);
    const auto rawBack = photon::proto::decodeHistoryChunk(rawPayload);
    CHECK(rawBack.has_value());
    CHECK(rawBack->bucketMs == 0);
    CHECK(rawBack->valueCount == 2);
    CHECK(rawBack->slotCount == 2);
    CHECK((rawBack->flags & photon::proto::kHistDense) != 0);
    CHECK(rawBack->timestamps.size() == 2);
    CHECK(rawBack->timestamps[1] == 20.5);
    CHECK(rawBack->rawV[1] == -3.5f);

    const auto frame = photon::proto::encodeFrame(photon::proto::MsgType::HistoryChunk, 3, payload);
    const auto decoded = photon::proto::decodeFrame(frame);
    CHECK(decoded.has_value());
    CHECK(decoded->header.msgType == 0x06);
}

void testListCatalogMatchesBoundsAndHash() {
    const auto root = uniqueDir("catalog-list");
    fs::create_directories(root);
    CHECK(tsdb::upsertCatalog(root, "20260904-090000-00", 1000, 50000, "deadbeef", nullptr));
    const auto listed = tsdb::listCatalog(root);
    CHECK(listed.size() == 1);
    CHECK(listed[0].id == "20260904-090000-00");
    CHECK(listed[0].minMs == 1000);
    CHECK(listed[0].maxMs == 50000);
    CHECK(listed[0].dbcSha == "deadbeef");
    CHECK(tsdb::listCatalog(root / "missing").empty());
}

}  // namespace

void runTsdbTests() {
    testPageCodec();
    testIndex();
    testTileAndLevel();
    testStoreRoundTrip();
    testBoundarySamples();
    testRangeBoundsAndLevels();
    testFoldKeepsGaps();
    testUnsignedRollupMinMaxMean();
    testSignedRollupWideSum();
    testEncodedFlushThreshold();
    testCodecWrap();
    testHistoryChunk();
    testIndexValidate();
    testListCatalogMatchesBoundsAndHash();
}
