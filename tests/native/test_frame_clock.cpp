#include "photon/canp.hpp"
#include "photon/dbc.hpp"
#include "photon/frame_clock.hpp"
#include "photon/tsdb.hpp"
#include "photon/vsd1.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

void runFrameClockTests();

namespace {

namespace fc = photon::frame_clock;
namespace fs = std::filesystem;

constexpr std::uint32_t kAccelFr = 4512;
constexpr std::uint32_t kGyroFr = 4513;
constexpr std::uint32_t kOther = 1234;

void addFrame(photon::canp::Batch& b, std::uint32_t id, std::uint16_t tick) {
    auto& p = b.packets[b.count++];
    p.canId = id;
    p.dlc = 8;
    p.data[0] = static_cast<std::uint8_t>(tick >> 8);
    p.data[1] = static_cast<std::uint8_t>(tick);
}

// A 20 ms batch as the car sends it: two IMU ticks, four accel samples and one
// gyro sample per tick, and an unrelated frame in between.
photon::canp::Batch carBatch(std::uint64_t ts, std::uint16_t firstTick) {
    photon::canp::Batch b{};
    b.timestamp = ts;
    for (int k = 0; k < 2; ++k) {
        const auto tick = static_cast<std::uint16_t>(firstTick + k);
        for (int j = 0; j < 4; ++j) {
            addFrame(b, kAccelFr, tick);
        }
        addFrame(b, kGyroFr, tick);
        addFrame(b, kOther, 0);
    }
    return b;
}

void testOtherFramesKeepBatchTime() {
    fc::FrameClock clock;
    std::uint64_t out[photon::canp::kMaxBatch];
    for (int i = 0; i < 5; ++i) {
        const auto b = carBatch(1000 + 20 * i, static_cast<std::uint16_t>(100 + 2 * i));
        clock.stamp(b, out);
        for (std::uint16_t p = 0; p < b.count; ++p) {
            if (b.packets[p].canId == kOther) {
                CHECK(out[p] == b.timestamp);
            }
        }
    }
}

void testSamplesSpreadInsideBatch() {
    fc::FrameClock clock;
    std::uint64_t out[photon::canp::kMaxBatch];
    // The first batch after a reset has no lower bound and keeps its time.
    auto first = carBatch(1000, 100);
    clock.stamp(first, out);
    for (std::uint16_t p = 0; p < first.count; ++p) {
        CHECK(out[p] == 1000);
    }
    std::map<std::uint32_t, std::uint64_t> last;
    for (int i = 1; i <= 50; ++i) {
        const std::uint64_t ts = 1000 + 20 * static_cast<std::uint64_t>(i);
        const auto b = carBatch(ts, static_cast<std::uint16_t>(100 + 2 * i));
        clock.stamp(b, out);
        for (std::uint16_t p = 0; p < b.count; ++p) {
            const auto id = b.packets[p].canId;
            if (id == kOther) {
                continue;
            }
            // Inside (previous batch, this batch], and one message never repeats a time.
            CHECK(out[p] > ts - 20);
            CHECK(out[p] <= ts);
            CHECK(out[p] > last[id]);
            last[id] = out[p];
        }
    }
}

void testCounterWrapAndRestart() {
    fc::FrameClock clock;
    std::uint64_t out[photon::canp::kMaxBatch];
    std::uint64_t ts = 5000;
    std::uint16_t tick = 65530;
    std::uint64_t lastGyro = 0;
    for (int i = 0; i < 10; ++i, ts += 20, tick = static_cast<std::uint16_t>(tick + 2)) {
        const auto b = carBatch(ts, tick);
        clock.stamp(b, out);
        for (std::uint16_t p = 0; p < b.count; ++p) {
            if (b.packets[p].canId == kGyroFr) {
                CHECK(out[p] >= lastGyro);
                CHECK(out[p] <= ts);
                lastGyro = out[p];
            }
        }
    }
    // The sensor restarts at zero: still inside the window, still in order.
    const auto restart = carBatch(ts, 0);
    clock.stamp(restart, out);
    for (std::uint16_t p = 0; p < restart.count; ++p) {
        CHECK(out[p] > ts - 20);
        CHECK(out[p] <= ts);
    }
    // Time going backwards resets the clock: the batch keeps its own time.
    const auto back = carBatch(1000, 7);
    clock.stamp(back, out);
    for (std::uint16_t p = 0; p < back.count; ++p) {
        CHECK(out[p] == 1000);
    }
}

// The July clip from the car: before, every IMU sample of a batch shared the
// batch time. Index it the way photon-index does, validate the store, and check
// the accel series now has one time per sample at the IMU's real spacing.
void testExampleClip() {
#ifdef PHOTON_EXAMPLES_DIR
    const fs::path clip = fs::path(PHOTON_EXAMPLES_DIR) / "brainerd-30s";
    std::error_code ec;
    if (!fs::exists(clip / "2026-07-23-03-30s.log", ec)) {
        std::fprintf(stdout, "frame_clock: example clip missing, skipped\n");
        return;
    }
    std::string error;
    const auto db = photon::dbc::loadFile((clip / "HighNoon.dbc").string(), &error);
    CHECK(db.has_value());
    if (!db) {
        return;
    }
    const auto out = fs::temp_directory_path() / "photon-frame-clock-tests" / "clip";
    fs::remove_all(out, ec);
    fs::create_directories(out, ec);
    const auto stats = photon::tsdb::indexLogs(clip, out, *db, "clip", &error);
    CHECK(error.empty());
    CHECK(stats.complete);

    photon::tsdb::Store store;
    CHECK(store.open(out / "clip", &error));
    CHECK(photon::tsdb::validateAgainstDecode(store, clip, *db, &error));
    CHECK(error.empty());

    const std::pair<const char*, const char*> signals[] = {{"IMU_Accel_FR", "Accel_X"}, {"IMU_Gyro_FR", "Gyro_X"}};
    for (const auto& [msg, name] : signals) {
        const auto id = photon::dbc::signalIdOf(msg, name);
        photon::tsdb::Query q;
        q.fromMs = store.manifest().minMs;
        q.toMs = store.manifest().maxMs;
        q.signalIds = {id};
        q.maxPoints = 0xffffffffu;
        const auto result = store.query(q);
        CHECK(result.series.size() == 1);
        if (result.series.size() != 1) {
            continue;
        }
        const auto& ts = result.series[0].timestamps;
        std::size_t repeats = 0;
        std::size_t backwards = 0;
        for (std::size_t i = 1; i < ts.size(); ++i) {
            repeats += ts[i] == ts[i - 1];
            backwards += ts[i] < ts[i - 1];
        }
        const double spanMs = ts.empty() ? 0.0 : static_cast<double>(ts.back() - ts.front());
        const double meanStep = ts.size() > 1 ? spanMs / static_cast<double>(ts.size() - 1) : 0.0;
        std::fprintf(stdout, "frame_clock clip %s: %zu samples, %zu repeated times, mean step %.2f ms\n", msg,
                     ts.size(), repeats, meanStep);
        CHECK(ts.size() > 2000);
        CHECK(backwards == 0);
        // Only the first batch keeps one shared time; everything after is spread.
        CHECK(repeats < ts.size() / 100);
    }
#else
    std::fprintf(stdout, "frame_clock: PHOTON_EXAMPLES_DIR not set, clip test skipped\n");
#endif
}

}  // namespace

void runFrameClockTests() {
    testOtherFramesKeepBatchTime();
    testSamplesSpreadInsideBatch();
    testCounterWrapAndRestart();
    testExampleClip();
}
