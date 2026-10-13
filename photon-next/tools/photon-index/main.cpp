#include "live_fanout.hpp"
#include "decode/dbc.hpp"
#include "store/tsdb.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <intrin.h>
#endif

namespace fs = std::filesystem;

void usage(const char* argv0) {
    std::fprintf(stdout,
                 "Usage: %s --logs DIR --out DIR [--session ID] [--dbc PATH] [--validate]\n"
                 "       %s --bench DIR [--bench-session ID] [--bench-seeks N] [--bench-signals K]\n"
                 "          [--bench-points P] [--bench-allow-debug]\n"
                 "       %s --append-bench DIR [--bench-session ID] [--append-bench-samples N]\n"
                 "       %s --live-bench DIR [--bench-session ID] [--bench-signals K]\n"
                 "          [--live-bench-samples N] [--bench-allow-debug]\n"
                 "       %s --retention DIR [--l0-max-age-ms N] [--l0-budget-bytes N] [--retention-apply]\n"
                 "       %s --retention-demo DIR [--bench-session ID]\n"
                 "       %s --live-fanout HOST:PORT --out DIR [--dbc PATH] [--video-vpt PATH] [--live-frames N]\n"
                 "  Index VSD1 logs into a Photon TSDB session (historical import only).\n"
                 "  --live-fanout writes the live TSDB from photon-server CANP fan-out (the PC ingest path).\n"
                 "  --bench measures seek latency: jump to a random time point and fill the window.\n"
                 "  --append-bench re-writes one real signal through Store::append and reports throughput.\n"
                 "  --live-bench replays real samples through the live tail writer, compares every query\n"
                 "     against the batch-built original, compacts, then drops L0 and reports what a query\n"
                 "     returns afterwards. Writes only to a temp directory; the corpus is read-only.\n"
                 "  --retention runs the retention policy over a corpus. DRY RUN unless --retention-apply.\n"
                 "  --retention-demo copies one real session, drops its L0, and reports what every signal\n"
                 "     answers before and after. The corpus copy is deleted afterwards.\n"
                 "  Seek, append and live benches refuse Debug builds unless --bench-allow-debug is set.\n",
                 argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

#ifndef PHOTON_STR
#define PHOTON_STR2(x) #x
#define PHOTON_STR(x) PHOTON_STR2(x)
#endif

const char* buildTypeName() {
#if defined(PHOTON_BUILD_TYPE)
    return PHOTON_STR(PHOTON_BUILD_TYPE);
#elif defined(NDEBUG)
    return "Release-or-RelWithDebInfo";
#else
    return "Debug";
#endif
}

bool isOptimizedBuild() {
#if defined(NDEBUG)
    return true;
#else
    return false;
#endif
}

bool refuseDebugBench(bool allowDebug) {
    std::fprintf(stdout, "[bench] build_type=%s NDEBUG=%s\n", buildTypeName(),
#if defined(NDEBUG)
                 "yes"
#else
                 "no"
#endif
    );
    if (isOptimizedBuild()) {
        return false;
    }
    std::fprintf(stderr,
                 "[bench] REFUSING Debug build. Latency and append numbers from Debug are not "
                 "comparable to the 25 ms warm-p95 target. Rebuild Release or RelWithDebInfo, "
                 "or pass --bench-allow-debug if you really want Debug.\n");
    if (allowDebug) {
        std::fprintf(stderr, "[bench] --bench-allow-debug set; continuing with Debug numbers.\n");
        return false;
    }
    return true;
}

void printCpuBrand() {
    char brand[49] = {};
#if defined(_WIN32)
    int info[4] = {};
    __cpuid(info, 0x80000000);
    if (static_cast<unsigned>(info[0]) >= 0x80000004u) {
        __cpuid(reinterpret_cast<int*>(brand + 0), 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
        brand[48] = 0;
    }
#endif
    if (brand[0] == 0) {
        std::fprintf(stdout, "[bench] cpu=(unavailable)\n");
        return;
    }
    const char* p = brand;
    while (*p == ' ') {
        ++p;
    }
    std::fprintf(stdout, "[bench] cpu=%s\n", p);
}

std::vector<std::uint8_t> readIndexedPage(const fs::path& sessionDir, int level, const photon::tsdb::IndexRecord& rec) {
    char name[16];
    std::snprintf(name, sizeof(name), "%04u.pcol", rec.fileNo);
    const auto path = sessionDir / ("l" + std::to_string(level)) / name;
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes;
    if (!in) {
        return bytes;
    }
    in.seekg(static_cast<std::streamoff>(rec.offset));
    bytes.resize(rec.pageBytes);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(rec.pageBytes));
    if (!in) {
        bytes.clear();
    }
    return bytes;
}

struct ChosenSession {
    fs::path dir;
    std::string id;
    std::uint64_t samples = 0;
};

ChosenSession chooseSession(const std::string& root, const std::string& pinId) {
    ChosenSession out;
    std::vector<fs::path> sessionDirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (entry.is_directory() && fs::exists(entry.path() / "session.json")) {
            sessionDirs.push_back(entry.path());
        }
    }
    std::sort(sessionDirs.begin(), sessionDirs.end());
    std::uint64_t biggestSamples = 0;
    for (const auto& dir : sessionDirs) {
        photon::tsdb::Store probe;
        if (!probe.open(dir)) {
            continue;
        }
        const auto& man = probe.manifest();
        if (!pinId.empty()) {
            if (man.id == pinId || dir.filename() == pinId) {
                out.dir = dir;
                out.id = man.id;
                out.samples = man.samples;
                return out;
            }
            continue;
        }
        if (man.samples > biggestSamples) {
            biggestSamples = man.samples;
            out.dir = dir;
            out.id = man.id;
            out.samples = man.samples;
        }
    }
    return out;
}

struct Percentiles {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
    double mean = 0.0;
};

Percentiles percentilesOf(std::vector<double> ms) {
    Percentiles p;
    if (ms.empty()) {
        return p;
    }
    std::sort(ms.begin(), ms.end());
    const auto at = [&](double q) {
        const auto idx = static_cast<std::size_t>(q * static_cast<double>(ms.size() - 1) + 0.5);
        return ms[std::min(idx, ms.size() - 1)];
    };
    p.p50 = at(0.50);
    p.p95 = at(0.95);
    p.p99 = at(0.99);
    p.max = ms.back();
    double sum = 0.0;
    for (const auto v : ms) {
        sum += v;
    }
    p.mean = sum / static_cast<double>(ms.size());
    return p;
}

// Seek benchmark. A seek is "jump the cursor to an arbitrary time and fill the
// whole window behind it", which is one range query per visible signal, so that
// is exactly what this times.
int runBench(const std::string& root, const std::string& pinId, int seeks, int signalCount,
             std::uint32_t maxPoints, bool allowDebug) {
    if (refuseDebugBench(allowDebug)) {
        return 2;
    }
    printCpuBrand();

    std::vector<fs::path> sessionDirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (entry.is_directory() && fs::exists(entry.path() / "session.json")) {
            sessionDirs.push_back(entry.path());
        }
    }
    if (sessionDirs.empty()) {
        std::fprintf(stderr, "[bench] no sessions under %s\n", root.c_str());
        return 1;
    }
    std::sort(sessionDirs.begin(), sessionDirs.end());

    const auto chosen = chooseSession(root, pinId);
    photon::tsdb::Store store;
    if (chosen.dir.empty() || !store.open(chosen.dir)) {
        if (!pinId.empty()) {
            std::fprintf(stderr, "[bench] cannot open pinned session %s\n", pinId.c_str());
        } else {
            std::fprintf(stderr, "[bench] cannot open a session\n");
        }
        return 1;
    }
    const auto& man = store.manifest();
    const auto spanMs = man.maxMs > man.minMs ? man.maxMs - man.minMs : 0;
    std::fprintf(stdout, "[bench] sessions=%zu, benching %s (%s)\n", sessionDirs.size(), man.id.c_str(),
                 pinId.empty() ? "largest by sample count" : "pinned");
    std::fprintf(stdout, "[bench] session_id=%s samples=%llu span=%.1f s levels=0x%02x\n", man.id.c_str(),
                 static_cast<unsigned long long>(man.samples), static_cast<double>(spanMs) / 1000.0,
                 static_cast<unsigned>(man.builtLevels));

    // Highest-rate signals: the most data to move per window.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> byPages;
    {
        const auto& idx = store.index(0);
        std::uint32_t cur = 0;
        std::uint64_t count = 0;
        for (const auto& r : idx) {
            if (r.signalId != cur) {
                if (cur != 0) {
                    byPages.emplace_back(count, cur);
                }
                cur = r.signalId;
                count = 0;
            }
            count += r.sampleCount;
        }
        if (cur != 0) {
            byPages.emplace_back(count, cur);
        }
    }
    std::sort(byPages.rbegin(), byPages.rend());
    std::vector<std::uint32_t> signals;
    for (const auto& [count, id] : byPages) {
        if (static_cast<int>(signals.size()) >= signalCount) {
            break;
        }
        (void)count;
        signals.push_back(id);
    }
    if (signals.empty()) {
        std::fprintf(stderr, "[bench] no signals in index\n");
        return 1;
    }
    std::fprintf(stdout, "[bench] %zu signals/query, %u points/signal, %d seeks per width\n", signals.size(),
                 maxPoints, seeks);
    std::fprintf(stdout, "[bench] seek_seed_base=0x5eed1234  (per-width seed is base+width_index)\n");
    std::fprintf(stdout,
                 "[bench] warm = pass 2: the same deterministic cursor vector replayed, not fresh targets. "
                 "cold = pass 1 of that vector. Target is warm p95 < 25 ms.\n");
    std::fprintf(stdout, "[bench] %-10s %-5s %8s %8s %8s %8s %8s %9s\n", "window", "pass", "p50ms", "p95ms", "p99ms",
                 "max ms", "mean ms", "pts/seek");

    const std::uint64_t widths[] = {1000, 2000, 10000, 60000, 600000, 7200000, spanMs};
    const char* names[] = {"1 s", "2 s", "10 s", "60 s", "600 s", "7200 s", "session"};
    constexpr std::size_t kWidthCount = sizeof(widths) / sizeof(widths[0]);
    int failures = 0;
    for (std::size_t w = 0; w < kWidthCount; ++w) {
        const auto width = std::min(widths[w], spanMs);
        if (width == 0) {
            continue;
        }
        // Same cursor sequence for both passes, so pass 2 measures cache reuse
        // of those exact seeks rather than a different set of targets.
        const std::uint32_t seekSeed = 0x5eed1234u + static_cast<std::uint32_t>(w);
        std::mt19937_64 rng(seekSeed);
        std::vector<std::uint64_t> cursors;
        cursors.reserve(seeks);
        for (int i = 0; i < seeks; ++i) {
            const auto usable = spanMs > width ? spanMs - width : 0;
            const auto off = usable > 0 ? rng() % usable : 0;
            cursors.push_back(man.minMs + width + off);
        }
        for (int pass = 1; pass <= 2; ++pass) {
            std::vector<double> lat;
            lat.reserve(cursors.size());
            std::uint64_t points = 0;
            int levelSeen = -1;
            bool mixedLevels = false;
            for (const auto cursor : cursors) {
                photon::tsdb::Query q;
                q.fromMs = cursor - width;
                q.toMs = cursor;
                q.signalIds = signals;
                q.maxPoints = maxPoints;
                const auto t0 = std::chrono::steady_clock::now();
                const auto res = store.query(q);
                const auto t1 = std::chrono::steady_clock::now();
                lat.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                for (const auto& s : res.series) {
                    points += s.timestamps.size() + s.buckets.size();
                    if (levelSeen < 0) {
                        levelSeen = s.level;
                    } else if (levelSeen != s.level) {
                        mixedLevels = true;
                    }
                }
            }
            const auto p = percentilesOf(lat);
            char levelText[24];
            std::snprintf(levelText, sizeof(levelText), "L%d%s", levelSeen < 0 ? 0 : levelSeen,
                          mixedLevels ? "+" : "");
            std::fprintf(stdout, "[bench] %-10s %-5d %8.2f %8.2f %8.2f %8.2f %8.2f %9llu  %s\n", names[w], pass,
                         p.p50, p.p95, p.p99, p.max, p.mean,
                         static_cast<unsigned long long>(points / static_cast<std::uint64_t>(seeks)), levelText);
            // Warm p95 target is 25 ms; cold is 150 ms.
            const double budget = pass == 1 ? 150.0 : 25.0;
            if (p.p95 > budget) {
                ++failures;
            }
        }
    }

    // Cross-session jumps include the store open, which a session switch pays.
    {
        std::vector<double> lat;
        std::mt19937_64 rng(0xabcdef01u);
        for (int i = 0; i < seeks; ++i) {
            const auto& dir = sessionDirs[rng() % sessionDirs.size()];
            const auto t0 = std::chrono::steady_clock::now();
            photon::tsdb::Store s;
            if (!s.open(dir)) {
                continue;
            }
            const auto& m = s.manifest();
            if (m.maxMs > m.minMs) {
                photon::tsdb::Query q;
                q.fromMs = m.maxMs > 60000 ? m.maxMs - 60000 : m.minMs;
                q.toMs = m.maxMs;
                q.signalIds = signals;
                q.maxPoints = maxPoints;
                (void)s.query(q);
            }
            const auto t1 = std::chrono::steady_clock::now();
            lat.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        const auto p = percentilesOf(lat);
        std::fprintf(stdout, "[bench] %-10s %-5s %8.2f %8.2f %8.2f %8.2f %8.2f\n", "switch", "open", p.p50, p.p95,
                     p.p99, p.max, p.mean);
    }
    std::fprintf(stdout, "[bench] %s\n", failures == 0 ? "all p95 within budget" : "SOME p95 OVER BUDGET");
    return 0;
}

int runAppendBench(const std::string& root, const std::string& pinId, std::uint64_t sampleCap, bool allowDebug) {
    if (refuseDebugBench(allowDebug)) {
        return 2;
    }
    printCpuBrand();
    const auto chosen = chooseSession(root, pinId);
    photon::tsdb::Store src;
    if (chosen.dir.empty() || !src.open(chosen.dir)) {
        std::fprintf(stderr, "[append-bench] cannot open session\n");
        return 1;
    }
    const auto& man = src.manifest();
    std::uint32_t bestId = 0;
    std::uint64_t bestCount = 0;
    {
        const auto& idx = src.index(0);
        std::uint32_t cur = 0;
        std::uint64_t count = 0;
        for (const auto& r : idx) {
            if (r.signalId != cur) {
                if (cur != 0 && count > bestCount) {
                    bestCount = count;
                    bestId = cur;
                }
                cur = r.signalId;
                count = 0;
            }
            count += r.sampleCount;
        }
        if (cur != 0 && count > bestCount) {
            bestCount = count;
            bestId = cur;
        }
    }
    if (bestId == 0) {
        std::fprintf(stderr, "[append-bench] no L0 signal in %s\n", man.id.c_str());
        return 1;
    }
    const auto* info = src.signal(bestId);
    std::fprintf(stdout, "[append-bench] session_id=%s samples=%llu pin=%s\n", man.id.c_str(),
                 static_cast<unsigned long long>(man.samples), pinId.empty() ? "largest" : "pinned");
    std::fprintf(stdout, "[append-bench] signal=%u name=%s l0_samples=%llu cap=%llu\n", bestId,
                 info != nullptr ? info->name.c_str() : "?", static_cast<unsigned long long>(bestCount),
                 static_cast<unsigned long long>(sampleCap));

    const auto tmp = fs::temp_directory_path() / ("photon-append-bench-" + man.id);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    photon::tsdb::Store dst;
    photon::tsdb::SessionManifest outMan;
    outMan.id = man.id + "-append-bench";
    std::string error;
    if (!dst.create(tmp, outMan, &error)) {
        std::fprintf(stderr, "[append-bench] create failed: %s\n", error.c_str());
        return 1;
    }
    if (info != nullptr) {
        dst.setSignal(*info);
    }

    std::uint64_t written = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& rec : src.index(0)) {
        if (rec.signalId != bestId) {
            continue;
        }
        const auto bytes = readIndexedPage(src.dir(), 0, rec);
        if (bytes.empty()) {
            continue;
        }
        photon::tsdb::PageHeader hdr{};
        std::vector<std::uint64_t> times;
        std::vector<std::int64_t> values;
        if (!photon::tsdb::decodeRawPage(bytes, hdr, times, values)) {
            continue;
        }
        const auto n = std::min(times.size(), values.size());
        for (std::size_t i = 0; i < n && written < sampleCap; ++i) {
            dst.append(bestId, times[i], values[i]);
            ++written;
        }
        if (written >= sampleCap) {
            break;
        }
    }
    if (!dst.finish(&error)) {
        std::fprintf(stderr, "[append-bench] finish failed: %s\n", error.c_str());
        dst.discard();
        return 1;
    }
    const auto t1 = std::chrono::steady_clock::now();
    dst.close();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double rate = secs > 0.0 ? static_cast<double>(written) / secs : 0.0;
    std::fprintf(stdout, "[append-bench] wrote=%llu elapsed=%.3f s rate=%.0f samples/s (build_type=%s)\n",
                 static_cast<unsigned long long>(written), secs, rate, buildTypeName());
    fs::remove_all(tmp, ec);
    return 0;
}

// ---------------------------------------------------------------------------
// Phase 5: live tail writer, compaction and retention, on real corpus data.
// ---------------------------------------------------------------------------

struct Triple {
    std::uint64_t t = 0;
    std::uint32_t id = 0;
    std::int64_t v = 0;
};

// Pull real L0 samples back out of a batch-built session and put them in arrival
// order, which is what a live feed delivers. The index is ordered by
// (signalId, startMs), so this has to sort.
std::vector<Triple> loadRealFeed(const photon::tsdb::Store& src, int signalCount, std::uint64_t cap,
                                 std::vector<std::uint32_t>& signalsOut) {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> byCount;
    {
        const auto& idx = src.index(0);
        std::uint32_t cur = 0;
        std::uint64_t count = 0;
        for (const auto& r : idx) {
            if (r.signalId != cur) {
                if (cur != 0) {
                    byCount.emplace_back(count, cur);
                }
                cur = r.signalId;
                count = 0;
            }
            count += r.sampleCount;
        }
        if (cur != 0) {
            byCount.emplace_back(count, cur);
        }
    }
    std::sort(byCount.rbegin(), byCount.rend());
    signalsOut.clear();
    for (const auto& [count, id] : byCount) {
        if (static_cast<int>(signalsOut.size()) >= signalCount) {
            break;
        }
        (void)count;
        signalsOut.push_back(id);
    }

    std::vector<Triple> feed;
    for (const auto id : signalsOut) {
        for (const auto& rec : src.index(0)) {
            if (rec.signalId != id) {
                continue;
            }
            const auto bytes = readIndexedPage(src.dir(), 0, rec);
            if (bytes.empty()) {
                continue;
            }
            photon::tsdb::PageHeader hdr{};
            std::vector<std::uint64_t> times;
            std::vector<std::int64_t> values;
            if (!photon::tsdb::decodeRawPage(bytes, hdr, times, values)) {
                continue;
            }
            const auto n = std::min(times.size(), values.size());
            for (std::size_t i = 0; i < n; ++i) {
                feed.push_back(Triple{times[i], id, values[i]});
            }
        }
    }
    std::stable_sort(feed.begin(), feed.end(), [](const Triple& a, const Triple& b) { return a.t < b.t; });
    if (feed.size() > cap) {
        feed.resize(static_cast<std::size_t>(cap));
    }
    return feed;
}

std::uint32_t countPages(const photon::tsdb::Store& store, int level) {
    return static_cast<std::uint32_t>(store.index(level).size());
}

// Same query against two stores must produce the same level, the same
// timestamps and values, and the same buckets. This is the real-data version of
// the live-vs-batch equality test in the suite.
int compareStores(const photon::tsdb::Store& a, const photon::tsdb::Store& b,
                  const std::vector<std::uint32_t>& signals, std::uint64_t minMs, std::uint64_t maxMs, int cases,
                  std::uint64_t maxWidth, int* casesRun) {
    std::mt19937_64 rng(0x11feb00cull);
    const std::uint64_t all[] = {500, 5000, 60000, 600000, maxMs > minMs ? maxMs - minMs : 1000};
    std::vector<std::uint64_t> widths;
    for (const auto w : all) {
        if (w <= maxWidth) {
            widths.push_back(w);
        }
    }
    if (widths.empty()) {
        widths.push_back(maxWidth);
    }
    const std::uint32_t points[] = {200, 2000, 20000};
    int mismatches = 0;
    int run = 0;
    for (int c = 0; c < cases; ++c) {
        const auto width = std::min(widths[rng() % widths.size()], maxMs > minMs ? maxMs - minMs : 1000);
        const auto usable = (maxMs - minMs) > width ? (maxMs - minMs) - width : 0;
        const auto from = minMs + (usable > 0 ? rng() % usable : 0);
        photon::tsdb::Query q;
        q.fromMs = from;
        q.toMs = from + width;
        q.signalIds = signals;
        q.maxPoints = points[rng() % 3];
        const auto ra = a.query(q);
        const auto rb = b.query(q);
        ++run;
        if (ra.series.size() != rb.series.size()) {
            ++mismatches;
            continue;
        }
        for (std::size_t i = 0; i < ra.series.size(); ++i) {
            const auto& sa = ra.series[i];
            const auto& sb = rb.series[i];
            bool bad = sa.signalId != sb.signalId || sa.level != sb.level ||
                       sa.timestamps.size() != sb.timestamps.size() || sa.raw.size() != sb.raw.size() ||
                       sa.buckets.size() != sb.buckets.size();
            for (std::size_t k = 0; !bad && k < sa.timestamps.size(); ++k) {
                bad = sa.timestamps[k] != sb.timestamps[k] || sa.raw[k] != sb.raw[k];
            }
            for (std::size_t k = 0; !bad && k < sa.buckets.size(); ++k) {
                const auto& x = sa.buckets[k];
                const auto& y = sb.buckets[k];
                bad = x.present != y.present || (x.present && (x.count != y.count || x.min != y.min ||
                                                               x.max != y.max || x.first != y.first ||
                                                               x.last != y.last || x.sum != y.sum));
            }
            if (bad) {
                ++mismatches;
                if (mismatches <= 5) {
                    std::fprintf(stderr,
                                 "[live-bench] MISMATCH signal=%u window=[%llu,%llu] maxPoints=%u level %d/%d "
                                 "raw %zu/%zu buckets %zu/%zu\n",
                                 sa.signalId, static_cast<unsigned long long>(q.fromMs),
                                 static_cast<unsigned long long>(q.toMs), q.maxPoints, sa.level, sb.level,
                                 sa.timestamps.size(), sb.timestamps.size(), sa.buckets.size(), sb.buckets.size());
                }
                break;
            }
        }
    }
    if (casesRun != nullptr) {
        *casesRun = run;
    }
    return mismatches;
}

int runLiveBench(const std::string& root, const std::string& pinId, int signalCount, std::uint64_t sampleCap,
                 bool allowDebug) {
    if (refuseDebugBench(allowDebug)) {
        return 2;
    }
    printCpuBrand();
    const auto chosen = chooseSession(root, pinId);
    photon::tsdb::Store src;
    if (chosen.dir.empty() || !src.open(chosen.dir)) {
        std::fprintf(stderr, "[live-bench] cannot open a session under %s\n", root.c_str());
        return 1;
    }
    const auto srcMan = src.manifest();
    std::fprintf(stdout, "[live-bench] source session=%s samples=%llu levels=0x%02x\n", srcMan.id.c_str(),
                 static_cast<unsigned long long>(srcMan.samples), static_cast<unsigned>(srcMan.builtLevels));

    std::vector<std::uint32_t> signals;
    const auto t0 = std::chrono::steady_clock::now();
    auto feed = loadRealFeed(src, signalCount, sampleCap, signals);
    const auto t1 = std::chrono::steady_clock::now();
    if (feed.empty()) {
        std::fprintf(stderr, "[live-bench] no L0 samples recovered from %s\n", srcMan.id.c_str());
        return 1;
    }
    std::fprintf(stdout, "[live-bench] feed: %zu samples over %u signals, span %.1f s (loaded in %.2f s)\n",
                 feed.size(), static_cast<unsigned>(signals.size()),
                 static_cast<double>(feed.back().t - feed.front().t) / 1000.0,
                 std::chrono::duration<double>(t1 - t0).count());

    const auto liveDir = fs::temp_directory_path() / ("photon-live-bench-" + srcMan.id);
    std::error_code ec;
    fs::remove_all(liveDir, ec);

    photon::tsdb::SessionManifest liveMan;
    liveMan.id = srcMan.id + "-live";
    liveMan.dbcSha = srcMan.dbcSha;
    liveMan.dbcLabel = srcMan.dbcLabel;
    photon::tsdb::LiveOptions opts;
    photon::tsdb::Store live;
    std::string error;
    if (!live.createLive(liveDir, liveMan, opts, &error)) {
        std::fprintf(stderr, "[live-bench] createLive failed: %s\n", error.c_str());
        return 1;
    }
    for (const auto id : signals) {
        if (const auto* info = src.signal(id)) {
            live.setSignal(*info);
        }
    }

    // A reader opened before the first sample: it must be able to see the
    // session grow, which is the whole point of the live tail.
    photon::tsdb::Store reader;
    if (!reader.open(liveDir, &error)) {
        std::fprintf(stderr, "[live-bench] a live session must be openable before its first sample: %s\n",
                     error.c_str());
        return 1;
    }

    // Feed loop. tick() after every sample is what the backend hook will do.
    const auto f0 = std::chrono::steady_clock::now();
    for (const auto& s : feed) {
        live.append(s.id, s.t, s.v);
        if (!live.tick(s.t, &error)) {
            std::fprintf(stderr, "[live-bench] tick failed: %s\n", error.c_str());
            return 1;
        }
    }
    const auto f1 = std::chrono::steady_clock::now();
    const double feedS = std::chrono::duration<double>(f1 - f0).count();
    const auto checkpoints = live.checkpointCount();

    // Mid-flight visibility, measured before finish() so it is the live path.
    const bool grew = reader.refresh(&error);
    const auto visibleAtEnd = reader.manifest().samples;

    if (!live.finish(&error)) {
        std::fprintf(stderr, "[live-bench] finish failed: %s\n", error.c_str());
        return 1;
    }
    const std::uint32_t livePages0 = countPages(live, 0);
    const std::uint64_t liveBytes = photon::tsdb::level0Bytes(liveDir);
    live.close();

    std::fprintf(stdout,
                 "[live-bench] live append: %zu samples in %.3f s = %.0f samples/s, %llu checkpoints "
                 "(%.1f ms apart in feed time), build_type=%s\n",
                 feed.size(), feedS, feedS > 0.0 ? static_cast<double>(feed.size()) / feedS : 0.0,
                 static_cast<unsigned long long>(checkpoints),
                 checkpoints > 0 ? static_cast<double>(feed.back().t - feed.front().t) /
                                       static_cast<double>(checkpoints)
                                 : 0.0,
                 buildTypeName());
    std::fprintf(stdout, "[live-bench] mid-flight reader: refresh_saw_growth=%s samples_visible=%llu of %zu\n",
                 grew ? "yes" : "no", static_cast<unsigned long long>(visibleAtEnd), feed.size());

    // Reference: the same real samples through the batch path. This is the
    // apples-to-apples check, because the live store holds exactly this feed.
    const auto batchDir = fs::temp_directory_path() / ("photon-live-bench-batch-" + srcMan.id);
    fs::remove_all(batchDir, ec);
    photon::tsdb::Store batch;
    photon::tsdb::SessionManifest batchMan;
    batchMan.id = srcMan.id + "-batch";
    if (!batch.create(batchDir, batchMan, &error)) {
        std::fprintf(stderr, "[live-bench] batch create failed: %s\n", error.c_str());
        return 1;
    }
    for (const auto id : signals) {
        if (const auto* info = src.signal(id)) {
            batch.setSignal(*info);
        }
    }
    const auto b0 = std::chrono::steady_clock::now();
    for (const auto& s : feed) {
        batch.append(s.id, s.t, s.v);
    }
    if (!batch.finish(&error)) {
        std::fprintf(stderr, "[live-bench] batch finish failed: %s\n", error.c_str());
        return 1;
    }
    const auto b1 = std::chrono::steady_clock::now();
    const double batchS = std::chrono::duration<double>(b1 - b0).count();
    const std::uint32_t batchPages0 = countPages(batch, 0);
    batch.close();
    std::fprintf(stdout, "[live-bench] batch append, same feed: %.3f s = %.0f samples/s (live is %.2fx slower)\n",
                 batchS, batchS > 0.0 ? static_cast<double>(feed.size()) / batchS : 0.0,
                 batchS > 0.0 ? feedS / batchS : 0.0);

    photon::tsdb::Store liveRead;
    photon::tsdb::Store batchRead;
    if (!liveRead.open(liveDir, &error) || !batchRead.open(batchDir, &error)) {
        std::fprintf(stderr, "[live-bench] reopen failed: %s\n", error.c_str());
        return 1;
    }
    const auto lo = feed.front().t;
    const auto hi = feed.back().t;
    int cases = 0;
    const int mismatches = compareStores(liveRead, batchRead, signals, lo, hi, 400, hi - lo, &cases);
    std::fprintf(stdout, "[live-bench] live vs batch, identical real feed: %d cases, %d mismatches\n", cases,
                 mismatches);

    // Against the untouched source session as well. The feed is a prefix of the
    // session, so only windows that end at least one L5 bucket before the feed
    // ends are covered by the same samples on both sides; wider ones would
    // differ at the tail bucket for reasons that are not a store bug.
    constexpr std::uint64_t kTailMargin = 600000;
    int srcCases = 0;
    int srcMismatches = -1;
    if (hi - lo > 2 * kTailMargin) {
        srcMismatches = compareStores(liveRead, src, signals, lo, hi - kTailMargin, 200, 60000, &srcCases);
        std::fprintf(stdout, "[live-bench] live vs untouched source session: %d cases, %d mismatches\n", srcCases,
                     srcMismatches);
    } else {
        std::fprintf(stdout, "[live-bench] live vs untouched source: skipped, feed span %.1f s is under 2x margin\n",
                     static_cast<double>(hi - lo) / 1000.0);
    }
    std::fprintf(stdout, "[live-bench] pages for the same feed: live L0=%u, batch L0=%u (%.1fx)\n", livePages0,
                 batchPages0, batchPages0 > 0 ? static_cast<double>(livePages0) / batchPages0 : 0.0);
    liveRead.close();
    batchRead.close();
    fs::remove_all(batchDir, ec);

    photon::tsdb::CompactionOptions copts;
    photon::tsdb::CompactionReport crep;
    if (!photon::tsdb::compactSession(liveDir, copts, crep, &error)) {
        std::fprintf(stderr, "[live-bench] compaction failed: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stdout, "[live-bench] compaction: %.3f s, %llu samples rewritten = %.0f samples/s\n", crep.elapsedS,
                 static_cast<unsigned long long>(crep.samplesRewritten),
                 crep.elapsedS > 0.0 ? static_cast<double>(crep.samplesRewritten) / crep.elapsedS : 0.0);
    for (int l = 0; l < photon::tsdb::kLevelCount; ++l) {
        if (crep.pagesBefore[l] == 0 && crep.pagesAfter[l] == 0) {
            continue;
        }
        std::fprintf(stdout, "[live-bench]   L%d pages %u -> %u\n", l, crep.pagesBefore[l], crep.pagesAfter[l]);
    }
    std::fprintf(stdout, "[live-bench] compaction bytes %llu -> %llu (%.1f%%)\n",
                 static_cast<unsigned long long>(crep.bytesBefore), static_cast<unsigned long long>(crep.bytesAfter),
                 crep.bytesBefore > 0
                     ? 100.0 * static_cast<double>(crep.bytesAfter) / static_cast<double>(crep.bytesBefore)
                     : 0.0);

    photon::tsdb::Store post;
    if (!post.open(liveDir, &error)) {
        std::fprintf(stderr, "[live-bench] reopen after compaction failed: %s\n", error.c_str());
        return 1;
    }
    photon::tsdb::Store batchRef;
    fs::remove_all(batchDir, ec);
    photon::tsdb::SessionManifest refMan;
    refMan.id = srcMan.id + "-ref";
    if (!batchRef.create(batchDir, refMan, &error)) {
        std::fprintf(stderr, "[live-bench] reference create failed: %s\n", error.c_str());
        return 1;
    }
    for (const auto id : signals) {
        if (const auto* info = src.signal(id)) {
            batchRef.setSignal(*info);
        }
    }
    for (const auto& s : feed) {
        batchRef.append(s.id, s.t, s.v);
    }
    if (!batchRef.finish(&error)) {
        std::fprintf(stderr, "[live-bench] reference finish failed: %s\n", error.c_str());
        return 1;
    }
    batchRef.close();
    photon::tsdb::Store refRead;
    if (!refRead.open(batchDir, &error)) {
        std::fprintf(stderr, "[live-bench] reference open failed: %s\n", error.c_str());
        return 1;
    }
    int cases2 = 0;
    const int mismatches2 = compareStores(post, refRead, signals, lo, hi, 400, hi - lo, &cases2);
    std::fprintf(stdout, "[live-bench] compacted vs batch, identical real feed: %d cases, %d mismatches\n", cases2,
                 mismatches2);
    refRead.close();
    fs::remove_all(batchDir, ec);

    // What a query returns before and after L0 is dropped, on this real data.
    const auto probeId = signals.front();
    const auto mid = lo + (hi - lo) / 2;
    photon::tsdb::Query pq;
    pq.signalIds = {probeId};
    pq.fromMs = mid;
    pq.toMs = mid + 10000;
    pq.maxPoints = 20000;
    const auto before = post.query(pq);
    const auto priorBefore = post.prior(probeId, mid);
    std::fprintf(stdout,
                 "[live-bench] retention probe signal=%u window=10 s: BEFORE level=L%d raw=%zu buckets=%zu "
                 "prior=%s(approx=%s)\n",
                 probeId, before.series.empty() ? -1 : before.series[0].level,
                 before.series.empty() ? 0 : before.series[0].timestamps.size(),
                 before.series.empty() ? 0 : before.series[0].buckets.size(),
                 priorBefore ? "yes" : "no", priorBefore && priorBefore->approximate ? "yes" : "no");
    post.close();

    std::uint32_t sealed = 0;
    std::uint64_t freed = 0;
    if (!photon::tsdb::dropLevel0(liveDir, true, &sealed, &freed, &error)) {
        std::fprintf(stderr, "[live-bench] dropLevel0 failed: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stdout, "[live-bench] dropLevel0: freed=%llu B of %llu B L0, sealed %u signal(s) into L5\n",
                 static_cast<unsigned long long>(freed), static_cast<unsigned long long>(liveBytes), sealed);

    photon::tsdb::Store dropped;
    if (!dropped.open(liveDir, &error)) {
        std::fprintf(stderr, "[live-bench] reopen after L0 drop failed: %s\n", error.c_str());
        return 1;
    }
    const auto after = dropped.query(pq);
    const auto priorAfter = dropped.prior(probeId, mid);
    std::fprintf(stdout,
                 "[live-bench] retention probe signal=%u window=10 s: AFTER  level=L%d raw=%zu buckets=%zu "
                 "prior=%s(approx=%s) l0Dropped=%s levels=0x%02x\n",
                 probeId, after.series.empty() ? -1 : after.series[0].level,
                 after.series.empty() ? 0 : after.series[0].timestamps.size(),
                 after.series.empty() ? 0 : after.series[0].buckets.size(), priorAfter ? "yes" : "no",
                 priorAfter && priorAfter->approximate ? "yes" : "no",
                 dropped.manifest().l0Dropped ? "yes" : "no",
                 static_cast<unsigned>(dropped.manifest().builtLevels));
    // A hole would be a bucket-less, sample-less answer over a window that has
    // data. Report per-signal coverage so that cannot hide.
    std::uint32_t emptySeries = 0;
    photon::tsdb::Query wide;
    wide.signalIds = signals;
    wide.fromMs = lo;
    wide.toMs = hi;
    wide.maxPoints = 2000;
    const auto wideRes = dropped.query(wide);
    for (const auto& s : wideRes.series) {
        std::uint64_t present = 0;
        for (const auto& b : s.buckets) {
            present += b.present ? 1 : 0;
        }
        if (s.timestamps.empty() && present == 0) {
            ++emptySeries;
            std::fprintf(stderr, "[live-bench] HOLE: signal=%u returned nothing after L0 drop\n", s.signalId);
        }
    }
    std::fprintf(stdout, "[live-bench] full-session query after L0 drop: %zu series, %u empty\n",
                 wideRes.series.size(), emptySeries);
    dropped.close();

    fs::remove_all(liveDir, ec);
    const bool ok = mismatches == 0 && mismatches2 == 0 && emptySeries == 0 && srcMismatches <= 0;
    std::fprintf(stdout, "[live-bench] %s\n", ok ? "OK" : "FAILURES ABOVE");
    return ok ? 0 : 1;
}

// Retention on a real session, every signal it has rather than the fast ones a
// bench would pick. Works on a copy, so the corpus is untouched.
int runRetentionDemo(const std::string& root, const std::string& pinId) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (entry.is_directory() && fs::exists(entry.path() / "session.json")) {
            dirs.push_back(entry.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());

    // Signals that hold L0 pages and nothing above are exactly the ones L0 drop
    // would erase, so they are what the sealPyramid path exists for. Pick the
    // session with the most of them; without one the demo cannot show sealing.
    const auto countAtRisk = [](const photon::tsdb::Store& s) {
        std::uint32_t atRisk = 0;
        for (const auto& [id, info] : s.signals()) {
            (void)info;
            const auto& l0 = s.index(0);
            const auto i = photon::tsdb::indexLowerBound(l0, id, 0);
            if (i >= l0.size() || l0[i].signalId != id) {
                continue;
            }
            bool above = false;
            for (int L = 1; L < photon::tsdb::kLevelCount && !above; ++L) {
                const auto& recs = s.index(L);
                const auto k = photon::tsdb::indexLowerBound(recs, id, 0);
                above = k < recs.size() && recs[k].signalId == id;
            }
            if (!above) {
                ++atRisk;
            }
        }
        return atRisk;
    };

    fs::path pick;
    std::uint32_t bestAtRisk = 0;
    std::uint64_t bestSamples = ~0ull;
    std::uint32_t corpusAtRisk = 0;
    std::uint32_t scanned = 0;
    for (const auto& d : dirs) {
        photon::tsdb::Store probe;
        if (!probe.open(d)) {
            continue;
        }
        ++scanned;
        if (!pinId.empty()) {
            if (probe.manifest().id == pinId || d.filename() == pinId) {
                pick = d;
                break;
            }
            continue;
        }
        const auto atRisk = countAtRisk(probe);
        corpusAtRisk += atRisk;
        const auto samples = probe.manifest().samples;
        if (samples == 0) {
            continue;
        }
        // Most at-risk signals wins; among equals the smallest, because the demo
        // copies the session.
        if (atRisk > bestAtRisk || (atRisk == bestAtRisk && samples < bestSamples)) {
            bestAtRisk = atRisk;
            bestSamples = samples;
            pick = d;
        }
    }
    if (pick.empty()) {
        std::fprintf(stderr, "[retention-demo] no session found under %s\n", root.c_str());
        return 1;
    }
    if (pinId.empty()) {
        std::fprintf(stdout,
                     "[retention-demo] scanned %u sessions, %u signal-sessions hold L0 and no rollup; "
                     "picked the session with %u of them\n",
                     scanned, corpusAtRisk, bestAtRisk);
    }

    const auto work = fs::temp_directory_path() / ("photon-retention-demo-" + pick.filename().string());
    fs::remove_all(work, ec);
    fs::copy(pick, work, fs::copy_options::recursive, ec);
    if (ec) {
        std::fprintf(stderr, "[retention-demo] copy failed: %s\n", ec.message().c_str());
        return 1;
    }

    std::string error;
    photon::tsdb::Store before;
    if (!before.open(work, &error)) {
        std::fprintf(stderr, "[retention-demo] open failed: %s\n", error.c_str());
        return 1;
    }
    const auto man = before.manifest();
    std::vector<std::uint32_t> allSignals;
    for (const auto& [id, s] : before.signals()) {
        (void)s;
        allSignals.push_back(id);
    }
    std::sort(allSignals.begin(), allSignals.end());
    std::fprintf(stdout, "[retention-demo] session=%s samples=%llu signals=%zu span=%.1f s levels=0x%02x\n",
                 man.id.c_str(), static_cast<unsigned long long>(man.samples), allSignals.size(),
                 static_cast<double>(man.maxMs - man.minMs) / 1000.0, static_cast<unsigned>(man.builtLevels));
    std::fprintf(stdout, "[retention-demo] signals with L0 only (no rollup at all): %u\n", [&] {
        std::uint32_t n = 0;
        for (const auto id : allSignals) {
            if (before.levelsOf(id) == 0x01) {
                ++n;
            }
        }
        return n;
    }());

    const auto mid = man.minMs + (man.maxMs - man.minMs) / 2;
    photon::tsdb::Query q;
    q.signalIds = allSignals;
    q.fromMs = mid;
    q.toMs = mid + 10000;
    q.maxPoints = 2000;

    // Which signals answer at all, per query, so "after" can be judged against
    // "before" instead of against nothing. Many signals in this corpus are
    // declared in the DBC and carry no samples in a given session; those were
    // empty before retention and are not holes retention created.
    const auto answered = [](const photon::tsdb::QueryResult& res) {
        std::vector<std::uint32_t> ids;
        for (const auto& sr : res.series) {
            std::uint64_t present = 0;
            for (const auto& b : sr.buckets) {
                present += b.present ? 1 : 0;
            }
            if (!sr.timestamps.empty() || present > 0) {
                ids.push_back(sr.signalId);
            }
        }
        return ids;
    };
    const auto describe = [&](const photon::tsdb::Store& s, const photon::tsdb::Query& query, const char* tag) {
        const auto res = s.query(query);
        std::uint32_t levelHist[photon::tsdb::kLevelCount]{};
        std::uint32_t empty = 0;
        std::uint32_t withPrior = 0;
        std::uint32_t approxPrior = 0;
        for (const auto& sr : res.series) {
            std::uint64_t present = 0;
            for (const auto& b : sr.buckets) {
                present += b.present ? 1 : 0;
            }
            if (sr.timestamps.empty() && present == 0) {
                ++empty;
            } else if (sr.level >= 0 && sr.level < photon::tsdb::kLevelCount) {
                ++levelHist[sr.level];
            }
            if (const auto p = s.prior(sr.signalId, mid)) {
                ++withPrior;
                if (p->approximate) {
                    ++approxPrior;
                }
            }
        }
        std::fprintf(stdout,
                     "[retention-demo] %-6s served L0=%u L1=%u L2=%u L3=%u L4=%u L5=%u  nothing=%u  "
                     "prior=%u (approximate=%u) of %zu signals\n",
                     tag, levelHist[0], levelHist[1], levelHist[2], levelHist[3], levelHist[4], levelHist[5], empty,
                     withPrior, approxPrior, res.series.size());
        return answered(res);
    };
    photon::tsdb::Query wide;
    wide.signalIds = allSignals;
    wide.fromMs = man.minMs;
    wide.toMs = man.maxMs;
    wide.maxPoints = 2000;
    const auto windowBefore = describe(before, q, "BEFORE");
    const auto wideBefore = answered(before.query(wide));
    std::fprintf(stdout, "[retention-demo] signals with data before the drop: %zu in the 10 s window, %zu session-wide\n",
                 windowBefore.size(), wideBefore.size());
    const auto l0 = photon::tsdb::level0Bytes(work);
    const auto total = [&] {
        std::uint64_t n = 0;
        for (const auto& e : fs::recursive_directory_iterator(work, ec)) {
            if (e.is_regular_file()) {
                n += e.file_size();
            }
        }
        return n;
    }();
    before.close();

    std::uint32_t sealed = 0;
    std::uint64_t freed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    if (!photon::tsdb::dropLevel0(work, true, &sealed, &freed, &error)) {
        std::fprintf(stderr, "[retention-demo] dropLevel0 failed: %s\n", error.c_str());
        return 1;
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::fprintf(stdout,
                 "[retention-demo] dropLevel0(seal=true): %.3f s, sealed=%u signal(s) into L5, "
                 "freed=%llu B of L0=%llu B, session %llu -> %llu B (%.1f%%)\n",
                 std::chrono::duration<double>(t1 - t0).count(), sealed, static_cast<unsigned long long>(freed),
                 static_cast<unsigned long long>(l0), static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(total - freed),
                 total > 0 ? 100.0 * static_cast<double>(total - freed) / static_cast<double>(total) : 0.0);

    photon::tsdb::Store after;
    if (!after.open(work, &error)) {
        std::fprintf(stderr, "[retention-demo] reopen failed: %s\n", error.c_str());
        return 1;
    }
    const auto windowAfter = describe(after, q, "AFTER");
    const auto wideAfter = answered(after.query(wide));
    std::fprintf(stdout, "[retention-demo] manifest l0Dropped=%s builtLevels=0x%02x\n",
                 after.manifest().l0Dropped ? "yes" : "no", static_cast<unsigned>(after.manifest().builtLevels));

    // A hole is a signal that answered before the drop and answers with nothing
    // after it. Anything already empty is a property of the recording.
    const auto regressions = [](const std::vector<std::uint32_t>& was, const std::vector<std::uint32_t>& is,
                                const char* scope) {
        std::uint32_t lost = 0;
        for (const auto id : was) {
            if (std::find(is.begin(), is.end(), id) == is.end()) {
                ++lost;
                if (lost <= 10) {
                    std::fprintf(stderr, "[retention-demo] HOLE: signal=%u answered %s before the drop, nothing after\n",
                                 id, scope);
                }
            }
        }
        return lost;
    };
    const auto lostWindow = regressions(windowBefore, windowAfter, "the 10 s window");
    const auto lostWide = regressions(wideBefore, wideAfter, "session-wide");
    std::fprintf(stdout, "[retention-demo] signals that lost all data: %u in the 10 s window, %u session-wide\n",
                 lostWindow, lostWide);
    after.close();
    fs::remove_all(work, ec);
    const bool ok = lostWindow == 0 && lostWide == 0;
    std::fprintf(stdout, "[retention-demo] %s\n", ok ? "OK, no holes" : "HOLES INTRODUCED");
    return ok ? 0 : 1;
}

int runRetention(const std::string& root, std::uint64_t maxAgeMs, std::uint64_t budgetBytes, bool apply) {
    photon::tsdb::RetentionPolicy policy;
    policy.l0MaxAgeMs = maxAgeMs;
    policy.l0BudgetBytes = budgetBytes;
    policy.sealPyramid = true;
    policy.dryRun = !apply;
    std::string error;
    // Wall clock, since retention ages a session against now, not feed time.
    const auto nowMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto report = photon::tsdb::applyRetention(root, policy, nowMs, &error);
    if (!report.ok) {
        std::fprintf(stderr, "[retention] failed: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stdout, "[retention] %s root=%s scanned=%u skipped_live=%u\n", apply ? "APPLY" : "DRY RUN",
                 root.c_str(), report.sessionsScanned, report.sessionsSkippedLive);
    std::fprintf(stdout, "[retention] l0_max_age=%llu ms l0_budget=%llu B l0_bytes_before=%llu B\n",
                 static_cast<unsigned long long>(maxAgeMs), static_cast<unsigned long long>(budgetBytes),
                 static_cast<unsigned long long>(report.l0BytesBefore));
    for (const auto& a : report.dropped) {
        std::fprintf(stdout, "[retention]   %s reason=%s age=%.1f h l0=%llu B sealed=%u\n", a.sessionId.c_str(),
                     a.reason.c_str(), static_cast<double>(a.ageMs) / 3600000.0,
                     static_cast<unsigned long long>(a.l0Bytes), a.sealedSignals);
    }
    std::fprintf(stdout, "[retention] would drop %zu session(s), freeing %llu B (%.2f GB)\n", report.dropped.size(),
                 static_cast<unsigned long long>(report.l0BytesFreed),
                 static_cast<double>(report.l0BytesFreed) / (1024.0 * 1024.0 * 1024.0));
    return report.ok ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string logs;
    std::string out;
    std::string session;
    std::string dbcPath;
    std::string benchRoot;
    std::string appendBenchRoot;
    std::string liveFanout;
    std::string videoVpt;
    std::string password;
    std::uint64_t liveFrames = 0;
    std::string liveBenchRoot;
    std::string retentionRoot;
    std::string retentionDemoRoot;
    std::string benchSession;
    std::uint64_t liveBenchSamples = 2'000'000;
    std::uint64_t l0MaxAgeMs = 0;
    std::uint64_t l0BudgetBytes = 0;
    bool retentionApply = false;
    int benchSeeks = 200;
    int benchSignals = 8;
    std::uint32_t benchPoints = 2000;
    std::uint64_t appendBenchSamples = 1'000'000;
    bool benchAllowDebug = false;
    bool validate = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](std::string& dest) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[photon-index] %s requires a value\n", arg.c_str());
                return false;
            }
            dest = argv[++i];
            return true;
        };
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (arg == "--logs") {
            if (!need(logs)) {
                return 2;
            }
        } else if (arg == "--out") {
            if (!need(out)) {
                return 2;
            }
        } else if (arg == "--session") {
            if (!need(session)) {
                return 2;
            }
        } else if (arg == "--dbc") {
            if (!need(dbcPath)) {
                return 2;
            }
        } else if (arg == "--live-fanout") {
            if (!need(liveFanout)) {
                return 2;
            }
        } else if (arg == "--video-vpt") {
            if (!need(videoVpt)) {
                return 2;
            }
        } else if (arg == "--password") {
            if (!need(password)) {
                return 2;
            }
        } else if (arg == "--live-frames") {
            std::string value;
            if (!need(value)) {
                return 2;
            }
            const auto n = std::atoll(value.c_str());
            if (n <= 0) {
                std::fprintf(stderr, "[photon-index] --live-frames needs a positive value\n");
                return 2;
            }
            liveFrames = static_cast<std::uint64_t>(n);
        } else if (arg == "--validate") {
            validate = true;
        } else if (arg == "--bench") {
            if (!need(benchRoot)) {
                return 2;
            }
        } else if (arg == "--append-bench") {
            if (!need(appendBenchRoot)) {
                return 2;
            }
        } else if (arg == "--live-bench") {
            if (!need(liveBenchRoot)) {
                return 2;
            }
        } else if (arg == "--retention") {
            if (!need(retentionRoot)) {
                return 2;
            }
        } else if (arg == "--retention-demo") {
            if (!need(retentionDemoRoot)) {
                return 2;
            }
        } else if (arg == "--retention-apply") {
            retentionApply = true;
        } else if (arg == "--bench-session") {
            if (!need(benchSession)) {
                return 2;
            }
        } else if (arg == "--bench-allow-debug") {
            benchAllowDebug = true;
        } else if (arg == "--append-bench-samples" || arg == "--live-bench-samples" || arg == "--l0-max-age-ms" ||
                   arg == "--l0-budget-bytes") {
            std::string value;
            if (!need(value)) {
                return 2;
            }
            const auto n = std::atoll(value.c_str());
            if (n <= 0) {
                std::fprintf(stderr, "[photon-index] %s needs a positive value\n", arg.c_str());
                return 2;
            }
            if (arg == "--append-bench-samples") {
                appendBenchSamples = static_cast<std::uint64_t>(n);
            } else if (arg == "--live-bench-samples") {
                liveBenchSamples = static_cast<std::uint64_t>(n);
            } else if (arg == "--l0-max-age-ms") {
                l0MaxAgeMs = static_cast<std::uint64_t>(n);
            } else {
                l0BudgetBytes = static_cast<std::uint64_t>(n);
            }
        } else if (arg == "--bench-seeks" || arg == "--bench-signals" || arg == "--bench-points") {
            std::string value;
            if (!need(value)) {
                return 2;
            }
            const auto n = std::atoi(value.c_str());
            if (n <= 0) {
                std::fprintf(stderr, "[photon-index] %s needs a positive value\n", arg.c_str());
                return 2;
            }
            if (arg == "--bench-seeks") {
                benchSeeks = n;
            } else if (arg == "--bench-signals") {
                benchSignals = n;
            } else {
                benchPoints = static_cast<std::uint32_t>(n);
            }
        } else {
            std::fprintf(stderr, "[photon-index] unknown flag %s\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    auto defaultDbc = [&]() {
        if (!dbcPath.empty()) {
            return;
        }
        if (const char* assets =
#if defined(_MSC_VER)
                []() -> const char* {
                    static char buf[512];
                    size_t n = 0;
                    return getenv_s(&n, buf, sizeof(buf), "PHOTON_ASSETS_DIR") == 0 && n > 1 ? buf : nullptr;
                }()
#else
                std::getenv("PHOTON_ASSETS_DIR")
#endif
        ) {
            dbcPath = (fs::path(assets) / "dbc" / "carcan_telemcan.dbc").string();
        } else {
            dbcPath = (fs::path(argv[0]).parent_path() / "assets" / "dbc" / "carcan_telemcan.dbc").string();
            if (!fs::exists(dbcPath)) {
                dbcPath = "F:/lhr/photon-next/assets/dbc/carcan_telemcan.dbc";
            }
        }
    };

    if (!liveFanout.empty()) {
        if (out.empty()) {
            std::fprintf(stderr, "[photon-index] --live-fanout requires --out\n");
            return 2;
        }
        defaultDbc();
        const auto colon = liveFanout.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= liveFanout.size()) {
            std::fprintf(stderr, "[photon-index] --live-fanout needs HOST:PORT\n");
            return 2;
        }
        LiveFanoutOptions live;
        live.host = liveFanout.substr(0, colon);
        live.port = static_cast<std::uint16_t>(std::atoi(liveFanout.c_str() + colon + 1));
        live.outDir = out;
        live.dbcPath = dbcPath;
        live.videoVptPath = videoVpt;
        live.maxFrames = liveFrames;
        live.password = password;
        if (live.password.empty()) {
#if defined(_MSC_VER)
            char buf[128];
            std::size_t n = 0;
            if (getenv_s(&n, buf, sizeof(buf), "PHOTON_PASSWORD") == 0 && n > 1) {
                live.password = buf;
            }
#else
            if (const char* env = std::getenv("PHOTON_PASSWORD")) {
                live.password = env;
            }
#endif
        }
        if (live.port == 0) {
            std::fprintf(stderr, "[photon-index] --live-fanout port is invalid\n");
            return 2;
        }
        return runLiveFanout(live);
    }

    if (!retentionDemoRoot.empty()) {
        return runRetentionDemo(retentionDemoRoot, benchSession);
    }
    if (!retentionRoot.empty()) {
        if (l0MaxAgeMs == 0 && l0BudgetBytes == 0) {
            std::fprintf(stderr, "[retention] give --l0-max-age-ms and/or --l0-budget-bytes\n");
            return 2;
        }
        return runRetention(retentionRoot, l0MaxAgeMs, l0BudgetBytes, retentionApply);
    }
    if (!liveBenchRoot.empty()) {
        return runLiveBench(liveBenchRoot, benchSession, benchSignals, liveBenchSamples, benchAllowDebug);
    }
    if (!appendBenchRoot.empty()) {
        return runAppendBench(appendBenchRoot, benchSession, appendBenchSamples, benchAllowDebug);
    }
    if (!benchRoot.empty()) {
        return runBench(benchRoot, benchSession, benchSeeks, benchSignals, benchPoints, benchAllowDebug);
    }
    if (logs.empty() || out.empty()) {
        usage(argv[0]);
        return 2;
    }

    defaultDbc();

    std::string error;
    auto db = photon::dbc::loadFile(dbcPath, &error);
    if (!db) {
        std::fprintf(stderr, "[photon-index] DBC load failed: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stdout, "[photon-index] dbc %s (%zu messages)\n", dbcPath.c_str(), db->messages.size());
    std::fprintf(stdout, "[photon-index] logs %s -> %s\n", logs.c_str(), out.c_str());

    const auto stats = photon::tsdb::indexLogs(logs, out, *db, session, &error);
    if (!stats.complete) {
        std::fprintf(stderr, "[photon-index] index failed after %zu session(s), %llu samples: %s\n",
                     stats.sessions.size(), static_cast<unsigned long long>(stats.samples),
                     error.empty() ? "(no error reported)" : error.c_str());
        return 1;
    }
    for (const auto& s : stats.sessions) {
        std::fprintf(stdout, "[photon-index] session %s frames=%llu samples=%llu store=%llu B\n", s.id.c_str(),
                     static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.samples),
                     static_cast<unsigned long long>(s.storeBytes));
    }
    std::fprintf(stdout,
                 "[photon-index] total sessions=%zu frames=%llu samples=%llu source=%llu B store=%llu B "
                 "elapsed=%.3f s\n",
                 stats.sessions.size(), static_cast<unsigned long long>(stats.frames),
                 static_cast<unsigned long long>(stats.samples), static_cast<unsigned long long>(stats.sourceBytes),
                 static_cast<unsigned long long>(stats.storeBytes), stats.elapsedS);
    if (stats.emptySessions > 0) {
        std::fprintf(stdout,
                     "[photon-index] skipped %u stretch(es), %llu frames, whose CAN ids match nothing in the DBC\n",
                     stats.emptySessions, static_cast<unsigned long long>(stats.undecodedFrames));
    }
    if (stats.sourceBytes > 0) {
        std::fprintf(stdout, "[photon-index] store/source ratio %.3f\n",
                     static_cast<double>(stats.storeBytes) / static_cast<double>(stats.sourceBytes));
        if (stats.samples > 0) {
            std::fprintf(stdout, "[photon-index] %.2f B/sample, %.2f MB/s\n",
                         static_cast<double>(stats.storeBytes) / static_cast<double>(stats.samples),
                         stats.elapsedS > 0.0
                             ? static_cast<double>(stats.sourceBytes) / stats.elapsedS / (1024.0 * 1024.0)
                             : 0.0);
        }
    }

    if (validate) {
        std::vector<photon::tsdb::Store> stores(stats.sessions.size());
        std::vector<const photon::tsdb::Store*> ptrs;
        ptrs.reserve(stores.size());
        for (std::size_t i = 0; i < stats.sessions.size(); ++i) {
            std::string openError;
            if (!stores[i].open(fs::path(out) / stats.sessions[i].id, &openError)) {
                std::fprintf(stderr, "[photon-index] validate open failed for %s: %s\n",
                             stats.sessions[i].id.c_str(), openError.c_str());
                return 1;
            }
            ptrs.push_back(&stores[i]);
        }
        std::string valError;
        if (!photon::tsdb::validateSessions(
                std::span<const photon::tsdb::Store* const>(ptrs.data(), ptrs.size()), logs, *db, &valError)) {
            std::fprintf(stderr, "[photon-index] validate failed: %s\n", valError.c_str());
            return 1;
        }
        std::fprintf(stdout, "[photon-index] validate ok, %zu sessions, %llu samples (raw triples vs live decode)\n",
                     ptrs.size(), static_cast<unsigned long long>(stats.samples));
    }
    return 0;
}
