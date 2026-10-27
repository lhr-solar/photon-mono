#include "photon/canp.hpp"
#include "photon/dbc.hpp"
#include "photon/tsdb.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

void runTsdbLiveTests();

namespace {

namespace fs = std::filesystem;
namespace tsdb = photon::tsdb;

fs::path uniqueDir(const char* tag) {
    const auto base = fs::temp_directory_path() / "photon-tsdb-live-tests";
    fs::create_directories(base);
    static int n = 0;
    const auto dir = base / (std::string(tag) + "-" + std::to_string(++n));
    // The name is stable across runs, and a live session refuses to open over an
    // existing session.json, so a previous run's leftovers would fail every test
    // here rather than exercising anything.
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

// Aligned to every bucket size in the pyramid (100 ms through 600 s), so the
// §6 boundary assertions below can name exact bucket starts.
constexpr std::uint64_t kBase = 1'700'000'400'000ull;

constexpr std::uint32_t kFastId = 1001;  //  50 Hz -> qualifies L1..L5
constexpr std::uint32_t kMidId = 1002;   //   1 Hz -> qualifies L3..L5
constexpr std::uint32_t kRareId = 1003;  // 2 samples 300 s apart -> no rollup

constexpr std::uint32_t kFastCount = 20000;  // 20 ms apart -> 400 s
constexpr std::uint32_t kFastStepMs = 20;
constexpr std::uint32_t kMidCount = 400;  // 1 s apart -> 400 s
constexpr std::uint32_t kMidStepMs = 1000;

std::int64_t fastRawAt(std::uint32_t i) {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(i) * 37ull) % 1000ull);
}

struct Sample {
    std::uint64_t t = 0;
    std::uint32_t id = 0;
    std::int64_t raw = 0;
};

std::vector<Sample> makeFeed(std::uint32_t fastCount, std::uint32_t midCount, bool includeRare) {
    std::vector<Sample> out;
    out.reserve(static_cast<std::size_t>(fastCount) + midCount + 2);
    for (std::uint32_t i = 0; i < fastCount; ++i) {
        out.push_back(Sample{kBase + static_cast<std::uint64_t>(i) * kFastStepMs, kFastId, fastRawAt(i)});
    }
    for (std::uint32_t i = 0; i < midCount; ++i) {
        out.push_back(Sample{kBase + static_cast<std::uint64_t>(i) * kMidStepMs, kMidId,
                             static_cast<std::int64_t>(i) - 200});
    }
    if (includeRare) {
        out.push_back(Sample{kBase, kRareId, 7});
        out.push_back(Sample{kBase + 300'000ull, kRareId, 9});
    }
    std::stable_sort(out.begin(), out.end(), [](const Sample& a, const Sample& b) { return a.t < b.t; });
    return out;
}

void declareSignals(tsdb::Store& store) {
    tsdb::SignalInfo fast;
    fast.id = kFastId;
    fast.message = "Fast";
    fast.name = "fast";
    fast.bits = 16;
    fast.isSigned = false;
    store.setSignal(fast);
    tsdb::SignalInfo mid;
    mid.id = kMidId;
    mid.message = "Mid";
    mid.name = "mid";
    mid.bits = 16;
    mid.isSigned = true;
    store.setSignal(mid);
    tsdb::SignalInfo rare;
    rare.id = kRareId;
    rare.message = "Rare";
    rare.name = "rare";
    rare.bits = 8;
    rare.isSigned = false;
    store.setSignal(rare);
}

bool buildBatch(const fs::path& dir, const std::vector<Sample>& feed, std::string& error) {
    tsdb::Store store;
    tsdb::SessionManifest man;
    man.id = dir.filename().string();
    if (!store.create(dir, man, &error)) {
        return false;
    }
    declareSignals(store);
    for (const auto& s : feed) {
        store.append(s.id, s.t, s.raw);
    }
    return store.finish(&error);
}

bool buildLive(const fs::path& dir, const std::vector<Sample>& feed, tsdb::LiveOptions opts, std::string& error,
               std::uint64_t* checkpoints = nullptr) {
    tsdb::Store store;
    tsdb::SessionManifest man;
    man.id = dir.filename().string();
    if (!store.createLive(dir, man, opts, &error)) {
        return false;
    }
    declareSignals(store);
    for (const auto& s : feed) {
        store.append(s.id, s.t, s.raw);
        if (!store.tick(s.t, &error)) {
            return false;
        }
    }
    if (checkpoints != nullptr) {
        *checkpoints = store.checkpointCount();
    }
    return store.finish(&error);
}

std::uint32_t pageCount(const tsdb::Store& store, int level) {
    return static_cast<std::uint32_t>(store.index(level).size());
}

bool sampleEqual(const std::optional<tsdb::RawSample>& a, const std::optional<tsdb::RawSample>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a.has_value()) {
        return true;
    }
    return a->timestampMs == b->timestampMs && a->raw == b->raw && a->approximate == b->approximate;
}

bool seriesEqual(const tsdb::SeriesResult& a, const tsdb::SeriesResult& b) {
    if (a.signalId != b.signalId || a.level != b.level || a.bucketMs != b.bucketMs) {
        return false;
    }
    if (a.timestamps != b.timestamps || a.raw != b.raw) {
        return false;
    }
    if (!sampleEqual(a.prior, b.prior) || !sampleEqual(a.next, b.next)) {
        return false;
    }
    if (a.buckets.size() != b.buckets.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.buckets.size(); ++i) {
        const auto& x = a.buckets[i];
        const auto& y = b.buckets[i];
        if (x.startMs != y.startMs || x.present != y.present || x.count != y.count) {
            return false;
        }
        if (!x.present || x.count == 0) {
            continue;
        }
        if (x.min != y.min || x.max != y.max || x.first != y.first || x.last != y.last || x.sum != y.sum ||
            x.sumHi != y.sumHi) {
            return false;
        }
    }
    return true;
}

struct RangeCase {
    std::uint64_t fromMs = 0;
    std::uint64_t toMs = 0;
    std::uint32_t maxPoints = 2000;
};

// A battery that exercises every level, both fold paths, zero-width ranges and
// ranges whose ends land exactly on bucket boundaries — which is where the §6
// intersection predicate is easiest to get wrong.
std::vector<RangeCase> rangeBattery(std::uint64_t minMs, std::uint64_t maxMs) {
    std::vector<RangeCase> cases;
    cases.push_back(RangeCase{minMs, maxMs, 2000});
    cases.push_back(RangeCase{minMs, maxMs, 7});
    cases.push_back(RangeCase{minMs, minMs, 2000});
    cases.push_back(RangeCase{maxMs, maxMs, 2000});
    cases.push_back(RangeCase{minMs + 1000, minMs + 1000, 2000});
    for (int L = 1; L <= 5; ++L) {
        const auto bms = tsdb::bucketMsOf(L);
        const auto start = ((minMs + bms * 3) / bms) * bms;
        cases.push_back(RangeCase{start, start + bms * 10, 10});
        cases.push_back(RangeCase{start, start + bms * 10 - 1, 10});
        cases.push_back(RangeCase{start + 1, start + bms * 10, 10});
        cases.push_back(RangeCase{start, start + bms * 40, 5});
    }
    std::mt19937_64 rng(0x9e3779b97f4a7c15ull);
    const std::uint32_t points[] = {1, 7, 100, 2000, 1'000'000};
    for (int i = 0; i < 200; ++i) {
        const auto span = maxMs > minMs ? maxMs - minMs : 1;
        const auto a = minMs + rng() % span;
        const auto width = 1 + rng() % span;
        cases.push_back(RangeCase{a, a + width, points[rng() % 5]});
    }
    return cases;
}

int compareBattery(const tsdb::Store& a, const tsdb::Store& b, const std::vector<std::uint32_t>& ids,
                   const std::vector<RangeCase>& cases) {
    int mismatches = 0;
    for (const auto& c : cases) {
        tsdb::Query q;
        q.fromMs = c.fromMs;
        q.toMs = c.toMs;
        q.signalIds = ids;
        q.maxPoints = c.maxPoints;
        const auto ra = a.query(q);
        const auto rb = b.query(q);
        if (ra.series.size() != rb.series.size()) {
            ++mismatches;
            continue;
        }
        for (std::size_t i = 0; i < ra.series.size(); ++i) {
            if (!seriesEqual(ra.series[i], rb.series[i])) {
                ++mismatches;
                if (mismatches <= 3) {
                    std::fprintf(stderr,
                                 "  mismatch signal=%u range=[%llu,%llu] pts=%u levels %d/%d sizes %zu+%zu / %zu+%zu\n",
                                 ra.series[i].signalId, static_cast<unsigned long long>(c.fromMs),
                                 static_cast<unsigned long long>(c.toMs), c.maxPoints, ra.series[i].level,
                                 rb.series[i].level, ra.series[i].timestamps.size(), ra.series[i].buckets.size(),
                                 rb.series[i].timestamps.size(), rb.series[i].buckets.size());
                }
            }
        }
    }
    return mismatches;
}

// --------------------------------------------------------------------------

void testLiveSessionIsQueryableWhileWriting() {
    const auto dir = uniqueDir("live-visible");
    tsdb::LiveOptions opts;
    opts.pageMaxAgeMs = 1000;
    opts.pageMinSamples = 64;
    opts.pageMaxIdleMs = 5000;
    opts.checkpointMs = 1000;
    opts.rateMinSamples = 16;
    opts.rateMinSpanMs = 1000;

    tsdb::Store writer;
    tsdb::SessionManifest man;
    man.id = "live-visible";
    std::string error;
    CHECK(writer.createLive(dir, man, opts, &error));
    CHECK(error.empty());
    declareSignals(writer);

    // Openable before a single sample: a live session is published at creation,
    // not by a rename after the car stops.
    tsdb::Store reader;
    CHECK(reader.open(dir, &error));
    CHECK(reader.manifest().live);
    CHECK(reader.manifest().samples == 0);
    const auto gen0 = reader.generation();

    const auto feed = makeFeed(kFastCount, kMidCount, true);
    const std::uint64_t head = kBase + 40'000ull;
    std::uint64_t fed = 0;
    for (const auto& s : feed) {
        if (s.t > head) {
            break;
        }
        writer.append(s.id, s.t, s.raw);
        CHECK(writer.tick(s.t, &error));
        ++fed;
    }
    CHECK(fed > 0);
    CHECK(writer.checkpointCount() >= 30);

    CHECK(reader.refresh(&error));
    CHECK(error.empty());
    CHECK(reader.generation() > gen0);
    CHECK(reader.manifest().samples > 0);
    CHECK(reader.manifest().samples <= fed);

    // Data well behind the feed head must be complete and exact.
    tsdb::Query q;
    q.fromMs = kBase + 10'000ull;
    q.toMs = kBase + 20'000ull;
    q.signalIds = {kFastId};
    q.maxPoints = 2000;
    const auto res = reader.query(q);
    CHECK(res.series.size() == 1);
    CHECK(res.series[0].level == 0);
    CHECK(res.series[0].timestamps.size() == 501);
    if (res.series[0].timestamps.size() == 501) {
        CHECK(res.series[0].timestamps.front() == kBase + 10'000ull);
        CHECK(res.series[0].timestamps.back() == kBase + 20'000ull);
        CHECK(res.series[0].raw.front() == fastRawAt(500));
        CHECK(res.series[0].raw.back() == fastRawAt(1000));
    }

    // Visibility lag: the newest queryable sample must be within one page-seal
    // window plus one checkpoint interval of the feed head. Waiting for a full
    // 4096-sample page would put this at ~81 s for this signal.
    tsdb::Query tail;
    tail.fromMs = head - 20'000ull;
    tail.toMs = head;
    tail.signalIds = {kFastId};
    tail.maxPoints = 1'000'000;
    const auto tailRes = reader.query(tail);
    CHECK(!tailRes.series[0].timestamps.empty());
    if (!tailRes.series[0].timestamps.empty()) {
        const auto newest = tailRes.series[0].timestamps.back();
        CHECK(newest <= head);
        CHECK(head - newest <= opts.pageMaxAgeMs + opts.checkpointMs + kFastStepMs);
    }

    // Nothing changed since the last refresh, so refresh reports no reload.
    CHECK(!reader.refresh(&error));

    CHECK(writer.finish(&error));
    CHECK(error.empty());
    CHECK(reader.refresh(&error));
    CHECK(!reader.manifest().live);
    writer.close();
}

void testLiveMatchesBatch() {
    const auto liveDir = uniqueDir("live-eq-live");
    const auto batchDir = uniqueDir("live-eq-batch");
    const auto feed = makeFeed(kFastCount, kMidCount, true);
    std::string error;

    tsdb::LiveOptions opts;
    opts.pageMaxAgeMs = 1000;
    opts.pageMinSamples = 64;
    opts.pageMaxIdleMs = 30'000;
    opts.checkpointMs = 1000;
    opts.rateMinSamples = 16;
    opts.rateMinSpanMs = 1000;
    std::uint64_t checkpoints = 0;
    CHECK(buildLive(liveDir, feed, opts, error, &checkpoints));
    CHECK(error.empty());
    CHECK(checkpoints >= 300);
    CHECK(buildBatch(batchDir, feed, error));
    CHECK(error.empty());

    tsdb::Store live;
    tsdb::Store batch;
    CHECK(live.open(liveDir, &error));
    CHECK(batch.open(batchDir, &error));
    CHECK(!live.manifest().live);
    CHECK(live.manifest().samples == batch.manifest().samples);
    CHECK(live.manifest().samples == feed.size());
    CHECK(live.manifest().builtLevels == batch.manifest().builtLevels);
    CHECK(live.levelsOf(kFastId) == batch.levelsOf(kFastId));
    CHECK(live.levelsOf(kMidId) == batch.levelsOf(kMidId));
    CHECK(live.levelsOf(kRareId) == batch.levelsOf(kRareId));

    // The live writer seals pages on age, so it produces many more, smaller
    // pages than a batch build of the same samples. That is the cost compaction
    // exists to reclaim.
    CHECK(pageCount(live, 0) > pageCount(batch, 0));

    const std::vector<std::uint32_t> ids{kFastId, kMidId, kRareId};
    const auto cases = rangeBattery(batch.manifest().minMs, batch.manifest().maxMs);
    const auto mismatches = compareBattery(live, batch, ids, cases);
    std::fprintf(stdout,
                 "tsdb live-vs-batch: %zu range cases, %d mismatches, live L0 pages=%u batch L0 pages=%u, "
                 "checkpoints=%llu\n",
                 cases.size(), mismatches, pageCount(live, 0), pageCount(batch, 0),
                 static_cast<unsigned long long>(checkpoints));
    CHECK(mismatches == 0);
}

void testLiveCrashRecovery() {
    const auto dir = uniqueDir("live-crash");
    const std::uint64_t orphanT = kBase + 900'000ull;
    std::uint64_t lastCheckpointedT = 0;
    std::string error;
    {
        tsdb::Store writer;
        tsdb::LiveOptions opts;
        opts.pageMaxAgeMs = 1000;
        opts.pageMinSamples = 64;
        opts.pageMaxIdleMs = 5000;
        opts.checkpointMs = 1000;
        opts.rateMinSamples = 16;
        opts.rateMinSpanMs = 1000;
        tsdb::SessionManifest man;
        man.id = "live-crash";
        CHECK(writer.createLive(dir, man, opts, &error));
        declareSignals(writer);
        const auto feed = makeFeed(kFastCount, kMidCount, false);
        for (const auto& s : feed) {
            if (s.t > kBase + 20'000ull) {
                break;
            }
            writer.append(s.id, s.t, s.raw);
            CHECK(writer.tick(s.t, &error));
        }
        CHECK(writer.checkpoint(kBase + 20'000ull, &error));
        lastCheckpointedT = kBase + 20'000ull;
        CHECK(writer.checkpointCount() >= 1);

        // Exactly one page's worth at a timestamp far past the last checkpoint,
        // with no tick, so the page reaches disk and no index ever names it.
        for (std::uint32_t i = 0; i < tsdb::kPageSampleCap; ++i) {
            writer.append(kFastId, orphanT, 4242);
        }
        // No finish(): the Store leaves scope with WRITER.lock on disk and
        // live=true in session.json, which is what a killed process looks like.
    }

    CHECK(fs::exists(dir / "WRITER.lock"));
    {
        tsdb::Store probe;
        CHECK(probe.open(dir, &error));
        CHECK(probe.manifest().live);
    }

    tsdb::RecoveryReport rep;
    CHECK(tsdb::recoverSession(dir, rep, &error));
    CHECK(error.empty());
    CHECK(rep.ranRecovery);
    CHECK(rep.filesTruncated == 1);
    CHECK(rep.filesRemoved == 0);
    CHECK(rep.recordsDropped == 0);
    CHECK(rep.bytesDiscarded > 0);
    CHECK(!fs::exists(dir / "WRITER.lock"));

    tsdb::Store after;
    CHECK(after.open(dir, &error));
    CHECK(!after.manifest().live);
    CHECK(after.generation() == rep.generation);

    // The checkpointed prefix survives intact.
    tsdb::Query kept;
    kept.fromMs = kBase + 5000ull;
    kept.toMs = kBase + 10'000ull;
    kept.signalIds = {kFastId};
    kept.maxPoints = 1'000'000;
    const auto keptRes = after.query(kept);
    CHECK(keptRes.series[0].timestamps.size() == 251);

    // The unindexed tail is gone rather than half-readable.
    tsdb::Query orphan;
    orphan.fromMs = orphanT - 1;
    orphan.toMs = orphanT + 1;
    orphan.signalIds = {kFastId};
    orphan.maxPoints = 1'000'000;
    const auto orphanRes = after.query(orphan);
    CHECK(orphanRes.series[0].timestamps.empty());
    CHECK(after.manifest().maxMs <= lastCheckpointedT);

    // Recovery is idempotent: a second pass finds nothing to do.
    tsdb::RecoveryReport again;
    CHECK(tsdb::recoverSession(dir, again, &error));
    CHECK(!again.ranRecovery);
    CHECK(again.bytesDiscarded == 0);
}

void testCompactionPreservesQueries() {
    const auto dir = uniqueDir("compact");
    // Deliberately pathological page sealing: one page per signal per 100 ms of
    // feed time, which is what a latency-hungry live writer produces.
    tsdb::LiveOptions opts;
    opts.pageMaxAgeMs = 100;
    opts.pageMinSamples = 1;
    opts.pageMaxIdleMs = 100;
    opts.checkpointMs = 500;
    opts.rateMinSamples = 16;
    opts.rateMinSpanMs = 1000;
    const auto feed = makeFeed(3000, 60, true);
    std::string error;
    CHECK(buildLive(dir, feed, opts, error));
    CHECK(error.empty());

    const std::vector<std::uint32_t> ids{kFastId, kMidId, kRareId};
    std::vector<tsdb::QueryResult> before;
    std::vector<RangeCase> cases;
    std::uint32_t pagesBefore0 = 0;
    {
        tsdb::Store store;
        CHECK(store.open(dir, &error));
        pagesBefore0 = pageCount(store, 0);
        CHECK(pagesBefore0 > 100);
        cases = rangeBattery(store.manifest().minMs, store.manifest().maxMs);
        before.reserve(cases.size());
        for (const auto& c : cases) {
            tsdb::Query q;
            q.fromMs = c.fromMs;
            q.toMs = c.toMs;
            q.signalIds = ids;
            q.maxPoints = c.maxPoints;
            before.push_back(store.query(q));
        }

        // Absolute §6 checks, so the before/after comparison cannot pass by
        // preserving a wrong answer.
        const auto bms = tsdb::bucketMsOf(1);
        const auto A = kBase + 5000ull;  // kBase is 100 ms aligned, so A is too
        tsdb::Query bq;
        bq.fromMs = A;
        bq.toMs = A + 950ull;
        bq.signalIds = {kFastId};
        bq.maxPoints = 10;
        const auto br = store.query(bq);
        CHECK(br.series[0].level == 1);
        CHECK(br.series[0].bucketMs == bms);
        CHECK(br.series[0].buckets.size() == 10);
        if (br.series[0].buckets.size() == 10) {
            // The bucket [A-100, A) ends exactly at A and is excluded.
            CHECK(br.series[0].buckets.front().startMs == A);
            // The last bucket runs past B; clients clip on display, not on data.
            CHECK(br.series[0].buckets.back().startMs == A + 900ull);
        }
        // L0 is inclusive on both ends.
        tsdb::Query rq;
        rq.fromMs = A;
        rq.toMs = A + 1000ull;
        rq.signalIds = {kFastId};
        rq.maxPoints = 1'000'000;
        const auto rr = store.query(rq);
        CHECK(rr.series[0].level == 0);
        CHECK(rr.series[0].timestamps.size() == 51);
        if (rr.series[0].timestamps.size() == 51) {
            CHECK(rr.series[0].timestamps.front() == A);
            CHECK(rr.series[0].timestamps.back() == A + 1000ull);
        }
    }

    tsdb::CompactionOptions copts;
    tsdb::CompactionReport rep;
    CHECK(tsdb::compactSession(dir, copts, rep, &error));
    CHECK(error.empty());
    CHECK(rep.pagesBefore[0] == pagesBefore0);
    CHECK(rep.pagesAfter[0] < rep.pagesBefore[0]);
    CHECK(rep.pagesAfter[0] > 0);
    CHECK(rep.pagesAfter[1] < rep.pagesBefore[1]);
    CHECK(rep.bytesAfter < rep.bytesBefore);
    CHECK(rep.samplesRewritten > 0);
    std::fprintf(stdout,
                 "tsdb compaction: L0 %u->%u pages, L1 %u->%u, bytes %llu->%llu (%.1f%%), %.3f s\n",
                 rep.pagesBefore[0], rep.pagesAfter[0], rep.pagesBefore[1], rep.pagesAfter[1],
                 static_cast<unsigned long long>(rep.bytesBefore), static_cast<unsigned long long>(rep.bytesAfter),
                 rep.bytesBefore > 0 ? 100.0 * static_cast<double>(rep.bytesAfter) /
                                           static_cast<double>(rep.bytesBefore)
                                     : 0.0,
                 rep.elapsedS);

    {
        tsdb::Store store;
        CHECK(store.open(dir, &error));
        CHECK(store.generation() == rep.generation);
        CHECK(pageCount(store, 0) == rep.pagesAfter[0]);
        int mismatches = 0;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            tsdb::Query q;
            q.fromMs = cases[i].fromMs;
            q.toMs = cases[i].toMs;
            q.signalIds = ids;
            q.maxPoints = cases[i].maxPoints;
            const auto after = store.query(q);
            if (after.series.size() != before[i].series.size()) {
                ++mismatches;
                continue;
            }
            for (std::size_t k = 0; k < after.series.size(); ++k) {
                if (!seriesEqual(before[i].series[k], after.series[k])) {
                    ++mismatches;
                }
            }
        }
        CHECK(mismatches == 0);
    }

    // Idempotent: with nothing left to merge, a second pass rewrites nothing.
    tsdb::CompactionReport rep2;
    CHECK(tsdb::compactSession(dir, copts, rep2, &error));
    CHECK(rep2.pagesAfter[0] == rep.pagesAfter[0]);
    CHECK(rep2.samplesRewritten == 0);
}

void testRetentionDropsL0AndDegrades() {
    const auto dir = uniqueDir("retain-drop");
    const auto feed = makeFeed(kFastCount, kMidCount, true);
    std::string error;
    CHECK(buildBatch(dir, feed, error));
    CHECK(error.empty());

    const auto atMs = kBase + 20'000ull;
    std::optional<tsdb::RawSample> priorBefore;
    std::optional<tsdb::RawSample> nextBefore;
    {
        tsdb::Store store;
        CHECK(store.open(dir, &error));
        CHECK(store.levelsOf(kFastId) == 0x3F);
        CHECK(store.levelsOf(kMidId) == 0x39);
        CHECK(store.levelsOf(kRareId) == 0x01);
        priorBefore = store.prior(kFastId, atMs);
        nextBefore = store.next(kFastId, atMs);
        CHECK(priorBefore.has_value());
        CHECK(!priorBefore->approximate);
        CHECK(priorBefore->timestampMs == atMs);
    }
    (void)nextBefore;

    const auto l0Before = tsdb::level0Bytes(dir);
    CHECK(l0Before > 0);
    std::uint32_t sealed = 0;
    std::uint64_t freed = 0;
    CHECK(tsdb::dropLevel0(dir, true, &sealed, &freed, &error));
    CHECK(error.empty());
    // Only the rare signal had nothing above L0, so only it needed a floor.
    CHECK(sealed == 1);
    CHECK(freed > 0);
    CHECK(tsdb::level0Bytes(dir) < l0Before);

    tsdb::Store after;
    CHECK(after.open(dir, &error));
    CHECK(after.manifest().l0Dropped);
    CHECK((after.manifest().builtLevels & 0x01) == 0);
    CHECK(after.index(0).empty());
    CHECK(after.levelsOf(kFastId) == 0x3E);
    CHECK(after.levelsOf(kMidId) == 0x38);
    CHECK(after.levelsOf(kRareId) == 0x20);

    // A window that used to be served raw now degrades to the finest level each
    // signal still has, per signal, rather than returning an empty series.
    tsdb::Query q;
    q.fromMs = kBase + 10'000ull;
    q.toMs = kBase + 20'000ull;
    q.signalIds = {kFastId, kMidId, kRareId};
    q.maxPoints = 2000;
    const auto res = after.query(q);
    CHECK(res.series.size() == 3);
    CHECK(res.series[0].level == 1);
    CHECK(res.series[0].timestamps.empty());
    CHECK(res.series[0].buckets.size() == 101);
    CHECK(res.series[1].level == 3);
    CHECK(!res.series[1].buckets.empty());
    CHECK(res.series[2].level == 5);
    CHECK(!res.series[2].buckets.empty());

    // No bucket of the degraded fast series may be empty: L1 at 100 ms over a
    // 50 Hz signal is 5 samples per bucket, and the 101 returned buckets run one
    // bucket past B because §6 keeps the partial edge bucket.
    std::uint32_t counted = 0;
    for (const auto& b : res.series[0].buckets) {
        CHECK(b.present);
        counted += b.count;
    }
    CHECK(counted == 505);

    // The rare signal keeps both of its samples at the floor level. Without the
    // pyramid seal it would have no level left at all.
    tsdb::Query rq;
    rq.fromMs = kBase;
    rq.toMs = kBase + 400'000ull;
    rq.signalIds = {kRareId};
    rq.maxPoints = 2000;
    const auto rres = after.query(rq);
    CHECK(rres.series[0].level == 5);
    std::uint32_t rareCount = 0;
    for (const auto& b : rres.series[0].buckets) {
        if (b.present) {
            rareCount += b.count;
        }
    }
    CHECK(rareCount == 2);

    // Boundary samples degrade to a bucket edge and say so.
    const auto priorAfter = after.prior(kFastId, atMs);
    CHECK(priorAfter.has_value());
    if (priorAfter) {
        CHECK(priorAfter->approximate);
        CHECK(priorAfter->timestampMs <= atMs);
        CHECK(atMs - priorAfter->timestampMs < tsdb::bucketMsOf(1));
        // Bucket [atMs-100, atMs) closes before atMs; its last sample is i=999.
        CHECK(priorAfter->raw == fastRawAt(999));
    }
    const auto nextAfter = after.next(kFastId, atMs);
    CHECK(nextAfter.has_value());
    if (nextAfter) {
        CHECK(nextAfter->approximate);
        CHECK(nextAfter->timestampMs >= atMs);
        CHECK(nextAfter->timestampMs == atMs);
        CHECK(nextAfter->raw == fastRawAt(1000));
    }

    // Idempotent.
    std::uint32_t sealed2 = 0;
    std::uint64_t freed2 = 0;
    CHECK(tsdb::dropLevel0(dir, true, &sealed2, &freed2, &error));
    CHECK(sealed2 == 0);
}

void testRetentionWithoutSealLosesTheSlowSignal() {
    const auto dir = uniqueDir("retain-noseal");
    const auto feed = makeFeed(kFastCount, kMidCount, true);
    std::string error;
    CHECK(buildBatch(dir, feed, error));
    std::uint32_t sealed = 0;
    CHECK(tsdb::dropLevel0(dir, false, &sealed, nullptr, &error));
    CHECK(sealed == 0);

    tsdb::Store after;
    CHECK(after.open(dir, &error));
    tsdb::Query q;
    q.fromMs = kBase;
    q.toMs = kBase + 400'000ull;
    q.signalIds = {kRareId, kFastId};
    q.maxPoints = 2000;
    const auto res = after.query(q);
    // Documented consequence of sealPyramid=false: a signal whose only level was
    // L0 goes dark until the session is re-indexed from VSD1 (§11).
    CHECK(res.series[0].buckets.empty());
    CHECK(res.series[0].timestamps.empty());
    CHECK(!res.series[0].prior.has_value());
    // Signals that do have a rollup are unaffected.
    CHECK(!res.series[1].buckets.empty());
}

void testRetentionPolicy() {
    const auto root = uniqueDir("retain-root");
    fs::create_directories(root);
    const std::uint64_t day = 86'400'000ull;
    const std::uint64_t nowMs = kBase + 30 * day;

    struct Built {
        std::string id;
        std::uint64_t base = 0;
    };
    const std::vector<Built> built{
        {"s-old", kBase},                 // ends ~20 days before now
        {"s-mid", kBase + 20 * day},      // ends ~10 days before now
        {"s-new", kBase + 29 * day},      // ends ~1 day before now
    };
    std::string error;
    for (const auto& b : built) {
        std::vector<Sample> feed;
        for (std::uint32_t i = 0; i < 2000; ++i) {
            feed.push_back(Sample{b.base + static_cast<std::uint64_t>(i) * 20ull, kFastId, fastRawAt(i)});
        }
        tsdb::Store store;
        tsdb::SessionManifest man;
        man.id = b.id;
        CHECK(store.create(root / b.id, man, &error));
        declareSignals(store);
        for (const auto& s : feed) {
            store.append(s.id, s.t, s.raw);
        }
        CHECK(store.finish(&error));
    }
    // A live session in the same root must be left alone: its writer owns it.
    tsdb::Store liveWriter;
    tsdb::SessionManifest liveMan;
    liveMan.id = "s-live";
    tsdb::LiveOptions lopts;
    CHECK(liveWriter.createLive(root / "s-live", liveMan, lopts, &error));
    declareSignals(liveWriter);
    for (std::uint32_t i = 0; i < 2000; ++i) {
        liveWriter.append(kFastId, kBase + static_cast<std::uint64_t>(i) * 20ull, fastRawAt(i));
        CHECK(liveWriter.tick(kBase + static_cast<std::uint64_t>(i) * 20ull, &error));
    }

    // Dry run touches nothing.
    tsdb::RetentionPolicy dry;
    dry.l0MaxAgeMs = 15 * day;
    dry.dryRun = true;
    const auto dryRep = tsdb::applyRetention(root, dry, nowMs, &error);
    CHECK(dryRep.ok);
    CHECK(dryRep.sessionsScanned == 4);
    CHECK(dryRep.sessionsSkippedLive == 1);
    CHECK(dryRep.dropped.size() == 1);
    if (dryRep.dropped.size() == 1) {
        CHECK(dryRep.dropped[0].sessionId == "s-old");
        CHECK(dryRep.dropped[0].reason == "age");
    }
    CHECK(tsdb::level0Bytes(root / "s-old") > 0);

    tsdb::RetentionPolicy age;
    age.l0MaxAgeMs = 15 * day;
    const auto ageRep = tsdb::applyRetention(root, age, nowMs, &error);
    CHECK(ageRep.ok);
    CHECK(error.empty());
    CHECK(ageRep.dropped.size() == 1);
    CHECK(ageRep.l0BytesFreed > 0);
    {
        tsdb::Store s;
        CHECK(s.open(root / "s-old", &error));
        CHECK(s.index(0).empty());
        CHECK(s.manifest().l0Dropped);
        tsdb::Store m;
        CHECK(m.open(root / "s-mid", &error));
        CHECK(!m.index(0).empty());
        CHECK(!m.manifest().l0Dropped);
    }

    // Budget rule: squeeze until the surviving L0 fits, oldest first. s-live is
    // unevictable, so its bytes stay in the total, and so does the empty index
    // an already-dropped session keeps.
    const auto liveL0 = tsdb::level0Bytes(root / "s-live");
    const auto oldL0 = tsdb::level0Bytes(root / "s-old");
    const auto midL0 = tsdb::level0Bytes(root / "s-mid");
    const auto newL0 = tsdb::level0Bytes(root / "s-new");
    CHECK(midL0 > 0);
    CHECK(newL0 > 0);
    tsdb::RetentionPolicy budget;
    budget.l0BudgetBytes = oldL0 + liveL0 + newL0;
    const auto budgetRep = tsdb::applyRetention(root, budget, nowMs, &error);
    CHECK(budgetRep.ok);
    CHECK(budgetRep.dropped.size() == 1);
    if (budgetRep.dropped.size() == 1) {
        CHECK(budgetRep.dropped[0].sessionId == "s-mid");
        CHECK(budgetRep.dropped[0].reason == "budget");
    }
    {
        tsdb::Store m;
        CHECK(m.open(root / "s-mid", &error));
        CHECK(m.index(0).empty());
        tsdb::Store n;
        CHECK(n.open(root / "s-new", &error));
        CHECK(!n.index(0).empty());
    }
    CHECK(tsdb::level0Bytes(root / "s-live") == liveL0);

    // A live session refuses an L0 drop outright, not just by policy.
    std::string dropError;
    CHECK(!tsdb::dropLevel0(root / "s-live", true, nullptr, nullptr, &dropError));
    CHECK(!dropError.empty());

    CHECK(liveWriter.finish(&error));
    liveWriter.close();
}

void testLiveFrameSinkDecodesRaw() {
    constexpr const char* kFixture = R"(
VERSION ""

BO_ 256 EngineData: 8 Vector
 SG_ RPM : 0|16@1+ (0.25,0) [0|8000] "rpm" Vector
 SG_ Temp : 16|8@1- (1,-40) [-40|215] "C" Vector
)";
    auto db = photon::dbc::parse(kFixture);
    CHECK(db.has_value());
    if (!db) {
        return;
    }
    const auto rpmId = photon::dbc::signalIdOf("EngineData", "RPM");
    const auto root = uniqueDir("live-sink");
    fs::create_directories(root);

    tsdb::LiveOptions opts;
    opts.pageMaxAgeMs = 100;
    opts.pageMinSamples = 1;
    opts.pageMaxIdleMs = 100;
    opts.checkpointMs = 200;
    tsdb::LiveTailWriter live;
    std::string error;
    CHECK(live.begin(root, "sink-1", *db, opts, &error));
    CHECK(error.empty());

    // Drive the sink through the IFrameSink interface, which is exactly the
    // shape the backend hook has to call.
    tsdb::IFrameSink& sink = live;
    std::vector<double> expected;
    for (int i = 0; i < 500; ++i) {
        photon::dbc::EncodedFrame frame{};
        const double rpm = 250.0 + static_cast<double>(i);
        CHECK(photon::dbc::encodeFrame(*db, "EngineData",
                                       std::vector<photon::dbc::SignalValue>{{"RPM", rpm}, {"Temp", 20.0}}, frame));
        sink.onFrame(frame.canId, frame.dlc, frame.data, kBase + static_cast<std::uint64_t>(i) * 50ull);
        expected.push_back(rpm);
    }
    CHECK(live.frames() == 500);
    CHECK(live.samples() == 1000);
    CHECK(!live.failed());
    CHECK(live.store().checkpointCount() > 0);
    CHECK(live.end(&error));
    CHECK(error.empty());

    tsdb::Store store;
    CHECK(store.open(root / "sink-1", &error));
    tsdb::Query q;
    q.fromMs = kBase;
    q.toMs = kBase + 500ull * 50ull;
    q.signalIds = {rpmId};
    q.maxPoints = 1'000'000;
    const auto res = store.query(q);
    CHECK(res.series.size() == 1);
    CHECK(res.series[0].level == 0);
    CHECK(res.series[0].timestamps.size() == 500);
    const auto* info = store.signal(rpmId);
    CHECK(info != nullptr);
    if (info != nullptr && res.series[0].raw.size() == 500) {
        // Raw integers scaled back through factor/offset must reproduce the
        // values the frames carried; a physical-domain hook would not.
        bool exact = true;
        for (std::size_t i = 0; i < 500; ++i) {
            exact = exact && near(tsdb::physical(*info, res.series[0].raw[i]), expected[i], 1e-9, 1e-9);
        }
        CHECK(exact);
    }
}

}  // namespace

void testUpsertCatalogAndRecoverWalk() {
    const auto root = uniqueDir("catalog-live");
    std::string error;
    CHECK(tsdb::upsertCatalog(root, "sess-a", 1000, 2000, "sha-a", &error));
    CHECK(error.empty());
    CHECK(tsdb::upsertCatalog(root, "sess-b", 3000, 4000, "sha-b", &error));
    CHECK(tsdb::upsertCatalog(root, "sess-a", 1000, 9000, "sha-a2", &error));

    {
        tsdb::Store writer;
        tsdb::LiveOptions opts;
        opts.pageMinSamples = 1;
        opts.pageMaxAgeMs = 100;
        opts.checkpointMs = 100;
        tsdb::SessionManifest man;
        man.id = "crash-walk";
        man.videoClockOffsetMs = -250;
        man.videoClockProv = "manual";
        CHECK(writer.createLive(root / "crash-walk", man, opts, &error));
        tsdb::SignalInfo info;
        info.id = kFastId;
        info.name = "fast";
        writer.setSignal(info);
        writer.append(kFastId, kBase, 1);
        CHECK(writer.checkpoint(kBase, &error));
        writer.setVideoClock(-250, "manual");
        CHECK(writer.checkpoint(kBase + 1, &error));
        // Leave live=true, same as a killed writer.
    }

    CHECK(tsdb::recoverCrashedSessions(root, &error) >= 1);
    tsdb::Store after;
    CHECK(after.open(root / "crash-walk", &error));
    CHECK(!after.manifest().live);
    CHECK(after.manifest().videoClockOffsetMs == -250);
    CHECK(after.manifest().videoClockProv == "manual");
}

void runTsdbLiveTests() {
    testLiveSessionIsQueryableWhileWriting();
    testLiveMatchesBatch();
    testLiveCrashRecovery();
    testCompactionPreservesQueries();
    testRetentionDropsL0AndDegrades();
    testRetentionWithoutSealLosesTheSlowSignal();
    testRetentionPolicy();
    testLiveFrameSinkDecodesRaw();
    testUpsertCatalogAndRecoverWalk();
}
