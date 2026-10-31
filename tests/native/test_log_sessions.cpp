#include "photon/log_sessions.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

void runLogSessionsTests();

namespace {

using photon::logsess::DetectorParams;
using photon::logsess::GpsSample;
using photon::logsess::SpeedSample;

std::vector<SpeedSample> hold(std::uint64_t t0, double seconds, double speed, double hz = 2.0) {
    std::vector<SpeedSample> out;
    const int n = static_cast<int>(seconds * hz) + 1;
    const auto dt = static_cast<std::uint64_t>(1000.0 / hz);
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        out.push_back(SpeedSample{t0 + static_cast<std::uint64_t>(i) * dt, speed});
    }
    return out;
}

void append(std::vector<SpeedSample>& dst, std::vector<SpeedSample> src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

}  // namespace

void runLogSessionsTests() {
    CHECK(photon::logsess::dateOf(1783893172546ull, -300) == "2026-07-12");
    // 2026-07-13 01:00 UTC is 2026-07-12 20:00 CDT; 05:00 UTC is local midnight.
    CHECK(photon::logsess::dateOf(1783904400000ull, -300) == "2026-07-12");
    CHECK(photon::logsess::dateOf(1783918800000ull, -300) == "2026-07-13");

    DetectorParams params;
    params.speedMinMps = 2.0;
    params.speedOffMps = 0.75;
    params.minMovingS = 45.0;
    params.mergeGapS = 180.0;
    params.padS = 15.0;
    params.holeS = 8.0;
    params.tzOffsetMin = -300;

    const std::uint64_t t0 = 1784000000000ull;

    auto idle = hold(t0, 120.0, 0.1);
    auto none = photon::logsess::detectSessions(idle, {}, params);
    CHECK(none.empty());

    auto burst = hold(t0, 20.0, 12.0);
    auto shortRun = photon::logsess::detectSessions(burst, {}, params);
    CHECK(shortRun.empty());

    auto stint = hold(t0, 60.0, 10.0);
    auto one = photon::logsess::detectSessions(stint, {}, params);
    CHECK(one.size() == 1);
    CHECK(one[0].startMs == t0 - 15000);
    CHECK(one[0].endMs == t0 + 60000 + 15000);
    CHECK(one[0].date == photon::logsess::dateOf(one[0].startMs, -300));
    CHECK(one[0].id == one[0].date + "-01");
    CHECK(one[0].speedMaxMps >= 9.9);
    CHECK(one[0].speedMeanMps >= 9.9);
    CHECK(photon::logsess::timestampInSessions(t0 + 1000, one));
    CHECK(!photon::logsess::timestampInSessions(t0 - 20000, one));

    std::vector<SpeedSample> twoClose;
    append(twoClose, hold(t0, 60.0, 11.0));
    append(twoClose, hold(t0 + 120000, 60.0, 9.0));
    auto merged = photon::logsess::detectSessions(twoClose, {}, params);
    CHECK(merged.size() == 1);
    CHECK(merged[0].endMs >= t0 + 180000);

    std::vector<SpeedSample> twoFar;
    append(twoFar, hold(t0, 60.0, 11.0));
    append(twoFar, hold(t0 + 600000, 60.0, 9.0));
    auto split = photon::logsess::detectSessions(twoFar, {}, params);
    CHECK(split.size() == 2);
    CHECK(split[0].id.size() >= 3);
    CHECK(split[1].startMs > split[0].endMs);

    // Crawl at 1.5 m/s is below enter (2.0) but above leave (0.75). Without
    // hysteresis a 20 s crawl after 8 s hole_s would close the run; merge_gap
    // 10 s would then emit two logs. Schmitt keeps one stint.
    DetectorParams hyst = params;
    hyst.minMovingS = 20.0;
    hyst.mergeGapS = 10.0;
    hyst.holeS = 8.0;
    std::vector<SpeedSample> crawl;
    append(crawl, hold(t0, 30.0, 3.0));
    append(crawl, hold(t0 + 30000, 20.0, 1.5));
    append(crawl, hold(t0 + 50000, 30.0, 3.0));
    auto oneCrawl = photon::logsess::detectSessions(crawl, {}, hyst);
    CHECK(oneCrawl.size() == 1);

    DetectorParams noHyst = hyst;
    noHyst.speedOffMps = 2.0;
    auto cut = photon::logsess::detectSessions(crawl, {}, noHyst);
    CHECK(cut.size() == 2);

    // Full stop still splits when the hole exceeds hole_s and merge_gap.
    std::vector<SpeedSample> stopped;
    append(stopped, hold(t0, 30.0, 3.0));
    append(stopped, hold(t0 + 30000, 20.0, 0.1));
    append(stopped, hold(t0 + 50000, 30.0, 3.0));
    auto twoStop = photon::logsess::detectSessions(stopped, {}, hyst);
    CHECK(twoStop.size() == 2);

    std::vector<GpsSample> gps{{t0 + 1000, 30.2672, -97.7431}, {t0 + 2000, 30.2674, -97.7430}};
    auto withGps = photon::logsess::detectSessions(stint, gps, params);
    CHECK(withGps.size() == 1);
    CHECK(withGps[0].haveGps);
    CHECK(near(withGps[0].gpsLat, 30.2673, 1e-3));

    auto catalog = photon::logsess::catalogFromSessions(split, params, "logs", "America/Chicago");
    CHECK(catalog.days.size() >= 1);
    CHECK(catalog.days[0].sessions.size() >= 1);
    const std::string json = photon::logsess::stringify(catalog);
    CHECK(json.find("\"days\"") != std::string::npos);
    std::string error;
    auto back = photon::logsess::parseCatalog(json, &error);
    CHECK(back.has_value());
    CHECK(error.empty());
    CHECK(back->days.size() == catalog.days.size());
    CHECK(back->days[0].sessions[0].startMs == catalog.days[0].sessions[0].startMs);
    CHECK(back->params.speedMinMps == params.speedMinMps);
    CHECK(back->params.speedOffMps == params.speedOffMps);

    namespace fs = std::filesystem;
    const auto tmp = fs::temp_directory_path() / "photon_drive_days_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "days" / "2026-07-18", ec);
    fs::create_directories(tmp / "days" / "2026-07-23", ec);
    {
        std::ofstream(tmp / "days" / "2026-07-18" / "run.log");
        std::ofstream(tmp / "days" / "2026-07-23" / "run.log");
    }
    {
        std::ofstream cat(tmp / "catalog.json");
        cat << R"({
  "version": 1,
  "source": "test",
  "timezone": "America/Chicago",
  "days": [
    {"date": "2026-07-18", "path": "days/2026-07-18", "duration_s": 83.0, "sessions": [{"id": "a"}]},
    {"date": "2026-07-23", "path": "days/2026-07-23", "duration_s": 16600.0, "sessions": [{"id": "b"}, {"id": "c"}]}
  ]
})";
    }
    CHECK(photon::logsess::directoryHasLogs((tmp / "days" / "2026-07-23").string()));
    CHECK(!photon::logsess::directoryHasLogs(tmp.string()));
    const auto days = photon::logsess::listDriveDays(tmp.string());
    CHECK(days.size() == 2);
    CHECK(days[0].date == "2026-07-18");
    CHECK(days[1].date == "2026-07-23");
    CHECK(days[1].sessionCount == 2);
    CHECK(days[1].sessions.size() == 2);
    CHECK(days[1].sessions[0].id == "b");
    const auto resolvedRoot = photon::logsess::resolveReplayPath(tmp.string());
    CHECK(resolvedRoot.find("2026-07-23") != std::string::npos);
    const auto resolvedNamed = photon::logsess::resolveReplayPath((tmp / "days" / "2026-07-18").string());
    CHECK(resolvedNamed.find("2026-07-18") != std::string::npos);
    fs::remove_all(tmp, ec);
}
