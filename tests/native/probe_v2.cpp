// Protocol v2 end-to-end probe: drives the real photon_ui::Client against a running
// photon-server and reports what the arena received. A console tool, not a ctest test.
//
//   photon-v2-probe <token> <seconds>                     subscribe to BPS_Status.Main_Battery_Voltage
//   photon-v2-probe <token> <seconds> --expect-refused    wrong token: expect the 1008 refusal, then no retry
//   photon-v2-probe <token> <seconds> --tiles             the live run's tiles through the real TileCache:
//                                                         one raw tile at the middle of the fed interval and the
//                                                         coarse tiles over it. The fed interval is the span of the
//                                                         live samples the probe sees, unless --window gives it:
//   photon-v2-probe <token> <seconds> --tiles --window <fromMs> <toMs>
//   photon-v2-probe <token> <seconds> --video [--replay-from <fromMs> <replaySeconds>]
//                                                         the live run's video for <seconds>; with --replay-from,
//                                                         the live run is pinned and played from fromMs, with a
//                                                         seek half way. Frames are taken from the arena as the
//                                                         decoder takes them; video_status lines print on change.
//
// Exit 0 on PASS, 1 on FAIL, 2 on bad arguments.

#include "photon/net.hpp"
#include "photon_ui/arena.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/platform.hpp"
#include "photon_ui/tile_cache.hpp"
#include "photon_ui/video_v2.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <thread>

namespace {

using photon::ui::Client;
using photon::ui::ClientArena;

constexpr const char* kSignalName = "BPS_Status.Main_Battery_Voltage";
constexpr double kExpectedV = 350.0;
constexpr double kCursorSlackMs = 1000.0;
constexpr double kQuietMs = 3000.0;

double wallMs() {
    using namespace std::chrono;
    return static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void printHello(const ClientArena& arena) {
    std::printf("hello_ack: helloOk=%d server=%s user=%s liveRun=%s clockOffsetNs=%lld levels=[",
                arena.server.helloOk ? 1 : 0, arena.server.server.c_str(), arena.server.user.c_str(),
                arena.server.liveRun.c_str(), static_cast<long long>(arena.server.clockOffsetNs));
    for (std::size_t i = 0; i < arena.server.levels.size(); ++i) {
        std::printf("%s%lld", i ? "," : "", static_cast<long long>(arena.server.levels[i]));
    }
    std::printf("]\nfeatures:");
    for (const auto& f : arena.features) {
        std::printf(" %s", f.c_str());
    }
    std::printf("\n");
}

void printLogTail(const ClientArena& arena, std::size_t lines) {
    const auto& logs = arena.logs();
    const std::size_t from = logs.size() > lines ? logs.size() - lines : 0;
    std::printf("log (last %zu of %zu):\n", logs.size() - from, logs.size());
    for (std::size_t i = from; i < logs.size(); ++i) {
        std::printf("  %s%s\n", logs[i].error ? "! " : "  ", logs[i].text.c_str());
    }
}

int runTest(ClientArena& arena, Client& client, double seconds) {
    const double start = photon::ui::nowMs();
    std::uint32_t id = 0;
    bool subscribed = false;
    bool signalMissing = false;
    double subscribedAt = 0.0;
    double firstSampleAt = -1.0;
    while (photon::ui::nowMs() - start < seconds * 1000.0) {
        client.tick(photon::ui::nowMs());
        if (!subscribed && arena.haveSignalCatalog) {
            for (const auto& sig : arena.signalCatalog) {
                if (sig.message + "." + sig.name == kSignalName) {
                    id = sig.id;
                    break;
                }
            }
            if (id == 0) {
                signalMissing = true;
                break;
            }
            client.ensureSubscribed(std::span<const std::uint32_t>(&id, 1));
            subscribed = true;
            subscribedAt = photon::ui::nowMs();
        }
        if (subscribed && firstSampleAt < 0.0) {
            const auto* ring = arena.ring(id);
            if (ring != nullptr && ring->count > 0) {
                firstSampleAt = photon::ui::nowMs();
            }
        }
        sleepMs(1);
    }

    std::printf("probe: url=%s state=%s seconds=%.1f\n", client.url().c_str(), client.stateLabel(), seconds);
    printHello(arena);

    std::printf("catalog: arrived=%d sha=%.12s signals=%zu buses=%zu first:",
                arena.haveSignalCatalog ? 1 : 0, arena.catalogSha.c_str(), arena.signalCatalog.size(),
                arena.catalogBuses.size());
    for (std::size_t i = 0; i < 3 && i < arena.signalCatalog.size(); ++i) {
        const auto& sig = arena.signalCatalog[i];
        std::printf(" [%u]%s.%s", static_cast<unsigned>(sig.id), sig.message.c_str(), sig.name.c_str());
    }
    std::printf("\n");

    if (signalMissing) {
        std::printf("subscribe: %s is not in the catalog\n", kSignalName);
        std::printf("RESULT: FAIL\n");
        printLogTail(arena, 25);
        return 1;
    }
    if (!subscribed) {
        std::printf("subscribe: no catalog within %.1f s\n", seconds);
        std::printf("RESULT: FAIL\n");
        printLogTail(arena, 25);
        return 1;
    }

    const auto* ring = arena.ring(id);
    const bool haveSamples = ring != nullptr && ring->count > 0;
    std::string unit;
    for (const auto& sig : arena.signalCatalog) {
        if (sig.id == id) {
            unit = sig.unit;
            break;
        }
    }
    const std::string firstSample =
        firstSampleAt < 0.0 ? std::string("none")
                            : std::to_string(static_cast<long long>(firstSampleAt - subscribedAt));
    std::printf("subscribe: name=%s id=%u index=%u unit=%s subscribed=%d first_sample_after_ms=%s\n",
                kSignalName, static_cast<unsigned>(id), static_cast<unsigned>(id - 1), unit.c_str(),
                client.subscribed().contains(id) ? 1 : 0, firstSample.c_str());
    if (haveSamples) {
        std::printf("ring: count=%zu write=%zu hasLatest=%d latest=%.9g latestTs=%.3f firstTs=%.3f lastTs=%.3f\n",
                    ring->count, ring->write, ring->hasLatest ? 1 : 0, ring->latest, ring->latestTs,
                    ring->firstTs(), ring->lastTs());
    } else {
        std::printf("ring: count=0 (no samples for %s)\n", kSignalName);
    }

    const double now = wallMs();
    const double cursor = static_cast<double>(arena.timeline().cursorMs);
    const bool cursorOk = std::fabs(now - cursor) <= kCursorSlackMs;
    std::printf("cursor: live=%d cursorMs=%.0f wall_ms=%.0f diff_ms=%.0f\n", arena.timelineIsLive() ? 1 : 0,
                cursor, now, now - cursor);
    std::printf("gaps: %zu\n", arena.gaps().size());

    const bool latestOk = haveSamples && ring->hasLatest && std::fabs(ring->latest - kExpectedV) < 1e-6;
    const bool ok = haveSamples && latestOk && cursorOk;
    std::printf("checks: samples=%d latest_350=%d cursor_within_1s=%d\n", haveSamples ? 1 : 0, latestOk ? 1 : 0,
                cursorOk ? 1 : 0);
    printLogTail(arena, 25);
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int runRefused(ClientArena& arena, Client& client, double seconds) {
    const double start = photon::ui::nowMs();
    double refusedAt = -1.0;
    std::size_t logsAtRefusal = 0;
    std::string lastAtRefusal;
    bool reconnected = false;
    while (true) {
        const double now = photon::ui::nowMs();
        client.tick(now);
        if (refusedAt < 0.0) {
            if (client.loginRefused()) {
                refusedAt = now;
                logsAtRefusal = arena.logs().size();
                lastAtRefusal = arena.logs().empty() ? std::string() : arena.logs().back().text;
            } else if (now - start >= seconds * 1000.0) {
                break;
            }
        } else {
            const bool logMoved = arena.logs().size() != logsAtRefusal ||
                                  (!arena.logs().empty() && arena.logs().back().text != lastAtRefusal);
            if (client.state() != Client::State::Disconnected || logMoved) {
                reconnected = true;
                break;
            }
            if (now - refusedAt >= kQuietMs) {
                break;
            }
        }
        sleepMs(1);
    }

    std::printf("probe: url=%s mode=expect-refused seconds=%.1f\n", client.url().c_str(), seconds);
    if (refusedAt < 0.0) {
        std::printf("refusal: none within %.1f s (state=%s)\n", seconds, client.stateLabel());
        std::printf("RESULT: FAIL\n");
        printLogTail(arena, 25);
        return 1;
    }
    std::string reason = "(no 'bad token' close line in the log)";
    for (const auto& line : arena.logs()) {
        if (line.text.find("bad token") != std::string::npos) {
            reason = line.text;
        }
    }
    std::printf("refusal: loginRefused=%d state=%d stateLabel=%s helloOk=%d\n", client.loginRefused() ? 1 : 0,
                static_cast<int>(client.state()), client.stateLabel(), arena.server.helloOk ? 1 : 0);
    std::printf("reason: %s\n", reason.c_str());
    std::printf("quiet: reconnected=%d window_ms=%.0f\n", reconnected ? 1 : 0, kQuietMs);
    const bool ok = client.loginRefused() && client.state() == Client::State::Disconnected &&
                    !arena.server.helloOk && !reconnected && reason.find("bad token") != std::string::npos;
    printLogTail(arena, 25);
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// --tiles: the tile path end to end. The client keeps the arena's TileCache on the
// live run (syncRun on every tick). This asks the cache for one raw tile at the middle
// of the fed interval and for the coarse tiles over the whole interval, then reports
// what the server sent back.
using photon::ui::Tile;
using photon::ui::TileCache;
using photon::ui::TileState;

constexpr double kTileWaitMs = 20000.0;        // every requested tile answered within this
constexpr double kMinFedSpanMs = 12000.0;      // live samples this wide give the fed interval (1000 px needs 10 s)
constexpr std::uint32_t kCoarsePixels = 1000;  // the coarsest level with a bucket of at most window / 1000 px
constexpr double kRawTolV = 0.01;              // every raw value must be this close to kExpectedV

int liveRunIndex(const ClientArena& arena) {
    for (std::size_t i = 0; i < arena.runs.size(); ++i) {
        if (arena.runs[i].live) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const char* tileStateName(TileState state) {
    switch (state) {
        case TileState::Requested:
            return "Requested";
        case TileState::Open:
            return "Open";
        case TileState::Sealed:
            return "Sealed";
        case TileState::Empty:
            return "Empty";
        case TileState::Unknown:
            return "Unknown";
    }
    return "?";
}

// A tile is settled once its request has an answer: data, no data, or a failure.
bool tileSettled(const TileCache& cache, std::uint32_t id, int level, std::int64_t index) {
    const Tile* tile = cache.find(id, level, index);
    return tile != nullptr && tile->state != TileState::Requested;
}

void printTile(const TileCache& cache, std::uint32_t id, int level, std::int64_t index) {
    const Tile* tile = cache.find(id, level, index);
    if (tile == nullptr) {
        std::printf("tile level=%d index=%lld: not in the cache\n", level, static_cast<long long>(index));
        return;
    }
    const std::size_t n = tile->t.size();
    std::printf("tile level=%d index=%lld state=%s sealed=%d samples=%zu", level, static_cast<long long>(index),
                tileStateName(tile->state), tile->sealed ? 1 : 0, n);
    if (n > 0) {
        std::printf(" first_ms=%.0f last_ms=%.0f", tile->t.front(), tile->t.back());
    }
    std::printf("\n");
    if (level == 0) {
        std::printf("  first5 value@t_ms:");
        for (std::size_t i = 0; i < n && i < 5 && i < tile->rawV.size(); ++i) {
            std::printf(" %.6g@%.0f", static_cast<double>(tile->rawV[i]), tile->t[i]);
        }
        std::printf("\n");
        if (!tile->rawV.empty()) {
            double lo = tile->rawV.front();
            double hi = tile->rawV.front();
            double sum = 0.0;
            std::size_t off = 0;
            for (const float v : tile->rawV) {
                lo = std::fmin(lo, static_cast<double>(v));
                hi = std::fmax(hi, static_cast<double>(v));
                sum += static_cast<double>(v);
                off += std::fabs(static_cast<double>(v) - kExpectedV) > kRawTolV ? 1 : 0;
            }
            std::printf("  raw min=%.6g max=%.6g mean=%.6g values_off_350=%zu\n", lo, hi,
                        sum / static_cast<double>(tile->rawV.size()), off);
        }
        return;
    }
    const std::size_t m = std::min({n, tile->minV.size(), tile->maxV.size(), tile->meanV.size()});
    std::printf("  first5 buckets t_ms min max mean:");
    for (std::size_t i = 0; i < m && i < 5; ++i) {
        std::printf(" [%.0f %.6g %.6g %.6g]", tile->t[i], static_cast<double>(tile->minV[i]),
                    static_cast<double>(tile->maxV[i]), static_cast<double>(tile->meanV[i]));
    }
    std::printf("\n");
    if (m > 0) {
        double lo = tile->minV.front();
        double hi = tile->maxV.front();
        double sum = 0.0;
        for (std::size_t i = 0; i < m; ++i) {
            lo = std::fmin(lo, static_cast<double>(tile->minV[i]));
            hi = std::fmax(hi, static_cast<double>(tile->maxV[i]));
            sum += static_cast<double>(tile->meanV[i]);
        }
        std::printf("  buckets=%zu all min=%.6g max=%.6g mean_of_means=%.6g\n", m, lo, hi,
                    sum / static_cast<double>(m));
    }
}

int runTiles(ClientArena& arena, Client& client, double seconds, bool haveWindow, std::uint64_t winFrom,
             std::uint64_t winTo) {
    const double start = photon::ui::nowMs();
    const double deadline = start + seconds * 1000.0;

    // 1. hello_ack, the catalog, and the runs list with the live run in it.
    bool runsRequested = false;
    while (photon::ui::nowMs() < deadline && !client.loginRefused()) {
        client.tick(photon::ui::nowMs());
        if (arena.server.helloOk && !runsRequested) {
            client.requestRuns();
            runsRequested = true;
        }
        if (arena.server.helloOk && arena.haveSignalCatalog && liveRunIndex(arena) >= 0) {
            break;
        }
        sleepMs(1);
    }
    std::printf("probe: url=%s state=%s mode=tiles seconds=%.1f\n", client.url().c_str(), client.stateLabel(), seconds);
    printHello(arena);
    std::printf("catalog: arrived=%d sha=%.12s signals=%zu\n", arena.haveSignalCatalog ? 1 : 0,
                arena.catalogSha.c_str(), arena.signalCatalog.size());
    std::printf("runs: %zu\n", arena.runs.size());
    for (const auto& run : arena.runs) {
        std::printf("  run id=%s live=%d start_ms=%lld end_ms=%lld rev=%lld dbc_sha=%.12s\n", run.id.c_str(),
                    run.live ? 1 : 0, static_cast<long long>(run.startMs), static_cast<long long>(run.endMs),
                    static_cast<long long>(run.rev), run.dbcSha.c_str());
    }
    const int live = liveRunIndex(arena);
    if (!arena.haveSignalCatalog || live < 0) {
        std::printf("live run: not listed within %.1f s (catalog=%d)\n", seconds, arena.haveSignalCatalog ? 1 : 0);
        printLogTail(arena, 8);
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    const std::string liveId = arena.runs[static_cast<std::size_t>(live)].id;
    const auto& liveRun = arena.runs[static_cast<std::size_t>(live)];
    std::printf("live run: id=%s start_ms=%lld end_ms=%lld rev=%lld sha=%.12s\n", liveId.c_str(),
                static_cast<long long>(liveRun.startMs), static_cast<long long>(liveRun.endMs),
                static_cast<long long>(liveRun.rev), liveRun.dbcSha.c_str());

    // 2. subscribe to the signal, as runTest does.
    std::uint32_t id = 0;
    for (const auto& sig : arena.signalCatalog) {
        if (sig.message + "." + sig.name == kSignalName) {
            id = sig.id;
            break;
        }
    }
    if (id == 0) {
        std::printf("subscribe: %s is not in the catalog\n", kSignalName);
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    client.ensureSubscribed(std::span<const std::uint32_t>(&id, 1));
    std::printf("subscribe: name=%s id=%u index=%u\n", kSignalName, static_cast<unsigned>(id),
                static_cast<unsigned>(id - 1));

    // 3. the fed interval: the --window given, else the span of the live samples seen here.
    std::uint64_t from = winFrom;
    std::uint64_t to = winTo;
    const char* source = "--window";
    if (!haveWindow) {
        source = "live samples";
        while (photon::ui::nowMs() < deadline) {
            client.tick(photon::ui::nowMs());
            const auto* seen = arena.ring(id);
            if (seen != nullptr && seen->count > 0 && seen->lastTs() - seen->firstTs() >= kMinFedSpanMs) {
                break;
            }
            sleepMs(1);
        }
        const auto* ring = arena.ring(id);
        if (ring == nullptr || ring->count == 0 || ring->lastTs() - ring->firstTs() < kMinFedSpanMs) {
            std::printf("window: no live samples spanning %.0f s within %.1f s (pass --window <fromMs> <toMs>)\n",
                        kMinFedSpanMs / 1000.0, seconds);
            printLogTail(arena, 8);
            std::printf("RESULT: FAIL\n");
            return 1;
        }
        from = static_cast<std::uint64_t>(ring->firstTs());
        to = static_cast<std::uint64_t>(ring->lastTs());
    }
    if (to <= from) {
        std::printf("window: empty (from_ms=%llu to_ms=%llu)\n", static_cast<unsigned long long>(from),
                    static_cast<unsigned long long>(to));
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    const std::uint64_t mid = from + (to - from) / 2;
    std::printf("window: source=%s from_ms=%llu to_ms=%llu span_ms=%llu mid_ms=%llu\n", source,
                static_cast<unsigned long long>(from), static_cast<unsigned long long>(to),
                static_cast<unsigned long long>(to - from), static_cast<unsigned long long>(mid));

    // 4. the cache must be on the live run before anything is asked of it.
    TileCache& cache = arena.tiles();
    const photon::ui::TileLevels& levels = cache.levels();
    std::printf("cache: run=%s rev=%lld sha=%.12s min_ms=%llu max_ms=%llu levels=%d\n", cache.runId().c_str(),
                static_cast<long long>(cache.rev()), cache.catalogSha().c_str(),
                static_cast<unsigned long long>(cache.runMinMs()), static_cast<unsigned long long>(cache.runMaxMs()),
                levels.count());
    if (cache.runId() != liveId || !levels.valid()) {
        std::printf("cache: not on the live run or no levels from hello_ack\n");
        printLogTail(arena, 8);
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    const std::uint32_t ids[1] = {id};
    const std::span<const std::uint32_t> signals(ids, 1);
    cache.requestCursorWindow(client, signals, mid);
    const int coarse = levels.chooseLevel(to - from, kCoarsePixels);
    cache.requestWindow(client, signals, from, to, kCoarsePixels, false);
    const std::int64_t rawIndex = levels.indexOf(0, mid);
    std::int64_t c0 = levels.indexOf(coarse, from);
    const std::int64_t c1 = levels.indexOf(coarse, to);
    if (c1 - c0 + 1 > TileCache::kMaxTilesPerWindow) {
        c0 = c1 - TileCache::kMaxTilesPerWindow + 1;
    }
    std::printf("requests: raw index=%lld (mid); coarse level=%d bucket_ms=%lld (%u px) index=%lld..%lld\n",
                static_cast<long long>(rawIndex), coarse,
                static_cast<long long>(coarse > 0 ? levels.bucketMs(coarse) : 0),
                static_cast<unsigned>(kCoarsePixels), static_cast<long long>(c0), static_cast<long long>(c1));

    // 5. pump the client and the cache until every requested tile has an answer.
    const double pumpStart = photon::ui::nowMs();
    const auto allSettled = [&] {
        if (!tileSettled(cache, id, 0, rawIndex)) {
            return false;
        }
        for (std::int64_t i = c0; i <= c1; ++i) {
            if (!tileSettled(cache, id, coarse, i)) {
                return false;
            }
        }
        return true;
    };
    while (photon::ui::nowMs() - pumpStart < kTileWaitMs) {
        client.tick(photon::ui::nowMs());
        cache.pump();
        if (allSettled()) {
            break;
        }
        sleepMs(2);
    }
    std::printf("settled: %d after %.0f ms\n", allSettled() ? 1 : 0, photon::ui::nowMs() - pumpStart);

    // 6. what came back.
    std::printf("-- raw (level 0)\n");
    printTile(cache, id, 0, rawIndex);
    std::printf("-- coarse (level %d)\n", coarse);
    for (std::int64_t i = c0; i <= c1; ++i) {
        printTile(cache, id, coarse, i);
    }

    const Tile* raw = cache.find(id, 0, rawIndex);
    const bool rawAnswered = raw != nullptr && (raw->state == TileState::Sealed || raw->state == TileState::Open) &&
                             !raw->t.empty() && raw->rawV.size() == raw->t.size();
    bool rawNear = rawAnswered;
    if (rawAnswered) {
        for (const float v : raw->rawV) {
            if (std::fabs(static_cast<double>(v) - kExpectedV) > kRawTolV) {
                rawNear = false;
                break;
            }
        }
    }
    std::size_t coarseSamples = 0;
    for (std::int64_t i = c0; i <= c1; ++i) {
        const Tile* tile = cache.find(id, coarse, i);
        coarseSamples += tile != nullptr ? tile->t.size() : 0;
    }
    std::printf("checks: raw_samples=%d raw_values_350=%d coarse_samples=%zu (coarse not gated)\n",
                rawAnswered ? 1 : 0, rawNear ? 1 : 0, coarseSamples);
    printLogTail(arena, 8);
    const bool ok = rawAnswered && rawNear;
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

// --video: the video of the live run, live and then replayed from a time, with a seek. Frames
// are taken from the arena's queue the way the decoder takes them; video_status lines print
// when the mode, the seek qid or the gap changes.
struct VideoTally {
    int frames = 0;
    int keys = 0;
    int configs = 0;
    std::uint64_t bytes = 0;
    std::set<std::uint64_t> times;      // distinct frame times
    std::set<std::uint64_t> keyTimes;   // distinct keyframe times
    std::uint64_t firstMs = 0;
    std::uint64_t lastMs = 0;
};

std::string flagText(std::uint8_t flags) {
    std::string out;
    if ((flags & photon::proto::kVideoFlagKeyframe) != 0) {
        out += "key";
    }
    if ((flags & photon::proto::kVideoFlagConfig) != 0) {
        out += out.empty() ? "cfg" : ",cfg";
    }
    return out.empty() ? "-" : out;
}

void drainVideo(ClientArena& arena, VideoTally& tally, const char* phase, int printFirst, int& printed) {
    for (const auto& ev : arena.takeVideoEvents()) {
        if (ev.kind == photon::ui::VideoEvent::Kind::Restart) {
            continue;
        }
        tally.firstMs = tally.frames == 0 ? ev.tMs : std::min(tally.firstMs, ev.tMs);
        tally.lastMs = std::max(tally.lastMs, ev.tMs);
        tally.times.insert(ev.tMs);
        if ((ev.flags & photon::proto::kVideoFlagKeyframe) != 0) {
            tally.keyTimes.insert(ev.tMs);
        }
        ++tally.frames;
        tally.bytes += ev.payload.size();
        if ((ev.flags & photon::proto::kVideoFlagKeyframe) != 0) {
            ++tally.keys;
        }
        if ((ev.flags & photon::proto::kVideoFlagConfig) != 0) {
            ++tally.configs;
        }
        if (printed < printFirst) {
            std::printf("%s frame t_ms=%llu flags=%s bytes=%zu\n", phase, static_cast<unsigned long long>(ev.tMs),
                        flagText(ev.flags).c_str(), ev.payload.size());
            ++printed;
        }
    }
}

// Prints the video status when what a viewer sees changed: the mode, the seek qid or the gap.
void printVideoStatus(const ClientArena& arena, std::string& lastKey) {
    if (!arena.video.haveStatus) {
        return;
    }
    std::string key = arena.video.mode;
    if (arena.video.qid) {
        key += " qid=" + std::to_string(*arena.video.qid);
    }
    if (arena.video.gap) {
        key += " gap=" + arena.video.gap->reason;
    }
    if (key == lastKey) {
        return;
    }
    lastKey = key;
    std::string line = "video_status mode=" + arena.video.mode + " t_ms=" + std::to_string(arena.video.tMs) +
                       " clock=" + arena.video.clock;
    if (arena.video.qid) {
        line += " qid=" + std::to_string(*arena.video.qid);
    }
    if (arena.video.gap) {
        line += " gap=" + arena.video.gap->reason + " from_ms=" + std::to_string(arena.video.gap->fromMs) +
                " to_ms=" + std::to_string(arena.video.gap->toMs) + " \"" +
                photon::ui::videoGapText(*arena.video.gap) + "\"";
    }
    std::printf("%s\n", line.c_str());
}

void printTally(const char* name, const VideoTally& t) {
    std::printf("%s: frames=%d keyframes=%d config=%d bytes=%llu\n", name, t.frames, t.keys, t.configs,
                static_cast<unsigned long long>(t.bytes));
    if (!t.times.empty()) {
        // Pictures per second over the span of the frame times: the camera rate, when the
        // stream is whole.
        const double spanS = static_cast<double>(t.lastMs - t.firstMs) / 1000.0;
        std::printf("%s: distinct t_ms=%zu span=%.2f s rate=%.1f per s, keyframe times=%zu\n", name, t.times.size(),
                    spanS, spanS > 0.0 ? static_cast<double>(t.times.size()) / spanS : 0.0, t.keyTimes.size());
    }
}

int runVideo(ClientArena& arena, Client& client, double liveSeconds, bool replay, std::uint64_t replayFrom,
             double replaySeconds) {
    VideoTally live;
    VideoTally rep;
    std::string lastKey;
    int printed = 0;
    std::printf("video: live for %.1f s\n", liveSeconds);
    const double liveStart = photon::ui::nowMs();
    while (photon::ui::nowMs() - liveStart < liveSeconds * 1000.0) {
        client.tick(photon::ui::nowMs());
        arena.markVideoWanted();  // no screen here: the probe is what wants video
        drainVideo(arena, live, "live", 6, printed);
        printVideoStatus(arena, lastKey);
        sleepMs(1);
    }
    printTally("live", live);
    const bool liveOk = live.frames > 0 && live.keys > 0;

    bool repOk = true;
    if (replay) {
        const photon::ctrl2::Run* target = nullptr;
        for (const auto& r : arena.runs) {
            if (r.live) {
                target = &r;
                break;
            }
        }
        if (target == nullptr && !arena.runs.empty()) {
            target = &arena.runs.front();
        }
        photon::ctrl2::Run liveByName;
        if (target == nullptr && !arena.server.liveRun.empty()) {
            // The server lists no runs (its history is not written), but hello_ack names the
            // live run. Pin it by that id, starting 60 s before now unless --replay-from gives a time.
            liveByName.id = arena.server.liveRun;
            liveByName.live = true;
            liveByName.startMs = static_cast<std::int64_t>(wallMs()) - 60000;
            target = &liveByName;
        }
        if (target == nullptr) {
            std::printf("video: no run to replay\n");
            return 1;
        }
        if (replayFrom == 0) {
            replayFrom = target->startMs > 0 ? static_cast<std::uint64_t>(target->startMs) : 0;
        }
        std::printf("video: replay run %s from t_ms=%llu for %.1f s\n", target->id.c_str(),
                    static_cast<unsigned long long>(replayFrom), replaySeconds);
        arena.pinRun(*target);
        arena.applyPinnedTimeline(photon::proto::TimelineMode::Play, replayFrom);
        const double repStart = photon::ui::nowMs();
        bool seeked = false;
        printed = 0;
        lastKey.clear();
        while (photon::ui::nowMs() - repStart < replaySeconds * 1000.0) {
            client.tick(photon::ui::nowMs());
            arena.markVideoWanted();
            arena.tickPinnedPlayback(photon::ui::nowMs());
            if (!seeked && photon::ui::nowMs() - repStart > replaySeconds * 500.0) {
                // A jump of 5 s while playing: the server is told where to go with video_seek.
                const std::uint64_t to = arena.timeline().cursorMs + 5000;
                std::printf("video: seek to t_ms=%llu\n", static_cast<unsigned long long>(to));
                arena.applyPinnedTimeline(photon::proto::TimelineMode::Play, to);
                seeked = true;
            }
            drainVideo(arena, rep, "replay", 6, printed);
            printVideoStatus(arena, lastKey);
            sleepMs(1);
        }
        printTally("replay", rep);
        repOk = rep.frames > 0 && rep.keys > 0;
        arena.unpinRun();
    }
    const bool pass = liveOk && repOk;
    std::printf("video result: %s (live %s%s)\n", pass ? "PASS" : "FAIL", liveOk ? "ok" : "no keyframes",
                replay ? (repOk ? ", replay ok" : ", replay no keyframes") : "");
    return pass ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: photon-v2-probe <token> <seconds> [--expect-refused | --tiles [--window <fromMs> <toMs>]]\n");
        return 2;
    }
    const std::string token = argv[1];
    const double seconds = std::atof(argv[2]);
    const bool expectRefused = argc > 3 && std::strcmp(argv[3], "--expect-refused") == 0;
    const bool tiles = argc > 3 && std::strcmp(argv[3], "--tiles") == 0;
    const bool video = argc > 3 && std::strcmp(argv[3], "--video") == 0;
    bool replay = false;
    std::uint64_t replayFrom = 0;
    double replaySeconds = 0.0;
    if (video) {
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--replay-from") == 0 && i + 2 < argc) {
                replay = true;
                replayFrom = std::strtoull(argv[i + 1], nullptr, 10);
                replaySeconds = std::atof(argv[i + 2]);
                i += 2;
            } else {
                std::fprintf(stderr, "usage: photon-v2-probe <token> <seconds> --video [--replay-from <fromMs> <replaySeconds>]\n");
                return 2;
            }
        }
    }
    // --window takes the fed interval from the caller instead of the live samples the probe sees.
    bool haveWindow = false;
    std::uint64_t winFrom = 0;
    std::uint64_t winTo = 0;
    if (tiles) {
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--window") == 0 && i + 2 < argc) {
                winFrom = std::strtoull(argv[i + 1], nullptr, 10);
                winTo = std::strtoull(argv[i + 2], nullptr, 10);
                haveWindow = true;
                i += 2;
            } else {
                std::fprintf(stderr, "usage: photon-v2-probe <token> <seconds> --tiles [--window <fromMs> <toMs>]\n");
                return 2;
            }
        }
        if (haveWindow && winTo <= winFrom) {
            std::fprintf(stderr, "--window needs fromMs < toMs\n");
            return 2;
        }
    }
    if (!(seconds > 0.0)) {
        std::fprintf(stderr, "seconds must be positive\n");
        return 2;
    }

    // Winsock starts in Guard's constructor; without it every socket call fails (as in ui/src/main.cpp).
    photon::net::Guard sockets;
    ClientArena arena;
    Client client(arena);
    client.setToken(token);
    client.setUrl("ws://127.0.0.1:6900/v2");
    const int rc = expectRefused ? runRefused(arena, client, seconds)
                 : tiles         ? runTiles(arena, client, seconds, haveWindow, winFrom, winTo)
                 : video         ? runVideo(arena, client, seconds, replay, replayFrom, replaySeconds)
                                 : runTest(arena, client, seconds);
    std::fflush(stdout);
    return rc;
}
