#pragma once

#include "base/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace photon::logsess {

// Detect on-track racing windows from vehicle/GPS speed so replay can drop
// garage, charge, and overnight idle. Sessions are grouped by local calendar
// day for a later "open this test day" picker.
//
// Algorithm (keep in sync with backend/scripts/extract_on_track.py):
//   1. Schmitt trigger on speed: enter a run at speed_min_mps, leave only
//      after speed stays < speed_off_mps (hysteresis so 1.5–2.5 m/s crawl
//      does not cut the log into many stints).
//   2. Adjacent moving samples form a run; a hole up to hole_s is still the
//      same run (dropped CAN frames).
//   3. Runs shorter than min_moving_s are discarded (shop bumps, roll tests).
//   4. Runs whose gap is <= merge_gap_s are one session (pit / brief stop).
//   5. Pad start/end by pad_s so launch and cooldown stay in the extract.

struct DetectorParams {
    double speedMinMps = 2.0;
    double speedOffMps = 0.75;  // leave-run threshold; <=0 means no hysteresis
    double minMovingS = 45.0;
    double mergeGapS = 180.0;
    double padS = 15.0;
    double holeS = 8.0;
    int tzOffsetMin = -300;  // America/Chicago CDT (July test logs)
};

struct SpeedSample {
    std::uint64_t timestampMs = 0;
    double speedMps = 0.0;
};

struct GpsSample {
    std::uint64_t timestampMs = 0;
    double lat = 0.0;
    double lon = 0.0;
};

struct Session {
    std::string id;
    std::string date;
    std::uint64_t startMs = 0;
    std::uint64_t endMs = 0;
    double durationS = 0.0;
    double speedMaxMps = 0.0;
    double speedMeanMps = 0.0;
    int speedSamples = 0;
    bool haveGps = false;
    double gpsLat = 0.0;
    double gpsLon = 0.0;
    double gpsLatMin = 0.0;
    double gpsLatMax = 0.0;
    double gpsLonMin = 0.0;
    double gpsLonMax = 0.0;
    std::string file;
};

struct Day {
    std::string date;
    std::string path;
    std::vector<Session> sessions;
    double durationS = 0.0;
};

struct Catalog {
    int version = 1;
    std::string source;
    std::string timezone = "America/Chicago";
    DetectorParams params;
    std::vector<Day> days;
};

std::string dateOf(std::uint64_t timestampMs, int tzOffsetMin);

std::vector<Session> detectSessions(const std::vector<SpeedSample>& speeds, const std::vector<GpsSample>& gps,
                                    const DetectorParams& params = {});

Catalog catalogFromSessions(std::vector<Session> sessions, DetectorParams params, std::string source,
                            std::string timezone = "America/Chicago");

bool timestampInSessions(std::uint64_t timestampMs, const std::vector<Session>& sessions);

json::Value toJson(const Catalog& catalog);
std::string stringify(const Catalog& catalog);
std::optional<Catalog> parseCatalog(std::string_view text, std::string* error = nullptr);

// catalog.json next to `path`, inside `path`, or in `path/on_track`.
std::optional<std::string> discoverCatalogPath(const std::string& path);

struct DriveSession {
    std::string id;
    std::string path;  // absolute .log if known
    std::uint64_t startMs = 0;
    double durationS = 0.0;
    double speedMaxMps = 0.0;
};

struct DriveDay {
    std::string date;
    std::string path;  // absolute folder that contains .log files
    int sessionCount = 0;
    double durationS = 0.0;
    std::vector<DriveSession> sessions;
};

bool directoryHasLogs(const std::string& path);

// Days from catalog.json next to `hintPath`. If hint is empty or has no
// catalog, fall back to PHOTON_ON_TRACK / `_logs_extract/on_track`.
std::vector<DriveDay> listDriveDays(const std::string& hintPath = {});

// If `path` already has .log files, return it. If it is a catalog root (or
// empty), return the matching day folder, else the longest day.
std::string resolveReplayPath(const std::string& path);

}  // namespace photon::logsess
