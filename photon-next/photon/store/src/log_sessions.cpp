#include "photon/log_sessions.hpp"
#include "photon/vsd1.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace photon::logsess {
namespace {

namespace fs = std::filesystem;

constexpr std::int64_t kMsPerDay = 86400000;

bool isLeap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

std::string formatDate(int year, int month, int day) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    return buf;
}

std::string sessionId(const std::string& date, int index) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%s-%02d", date.c_str(), index);
    return buf;
}

std::uint64_t clampSub(std::uint64_t value, std::uint64_t delta) {
    return value > delta ? value - delta : 0;
}

bool inRange(std::uint64_t ts, std::uint64_t startMs, std::uint64_t endMs) {
    return ts >= startMs && ts <= endMs;
}

void attachGps(Session& session, const std::vector<GpsSample>& gps) {
    double latSum = 0.0;
    double lonSum = 0.0;
    int n = 0;
    for (const auto& g : gps) {
        if (!inRange(g.timestampMs, session.startMs, session.endMs)) {
            continue;
        }
        if (n == 0) {
            session.gpsLatMin = session.gpsLatMax = g.lat;
            session.gpsLonMin = session.gpsLonMax = g.lon;
        } else {
            session.gpsLatMin = std::min(session.gpsLatMin, g.lat);
            session.gpsLatMax = std::max(session.gpsLatMax, g.lat);
            session.gpsLonMin = std::min(session.gpsLonMin, g.lon);
            session.gpsLonMax = std::max(session.gpsLonMax, g.lon);
        }
        latSum += g.lat;
        lonSum += g.lon;
        ++n;
    }
    if (n == 0) {
        return;
    }
    session.haveGps = true;
    session.gpsLat = latSum / static_cast<double>(n);
    session.gpsLon = lonSum / static_cast<double>(n);
}

void attachSpeedStats(Session& session, const std::vector<SpeedSample>& speeds, std::uint64_t rawStart,
                      std::uint64_t rawEnd) {
    double sum = 0.0;
    double mx = 0.0;
    int n = 0;
    for (const auto& s : speeds) {
        if (!inRange(s.timestampMs, rawStart, rawEnd) || s.speedMps < 0.0) {
            continue;
        }
        sum += s.speedMps;
        mx = std::max(mx, s.speedMps);
        ++n;
    }
    session.speedSamples = n;
    session.speedMaxMps = mx;
    session.speedMeanMps = n > 0 ? sum / static_cast<double>(n) : 0.0;
}

DetectorParams readParams(const json::Value& obj) {
    DetectorParams p;
    if (!obj.isObject()) {
        return p;
    }
    if (obj.contains("speed_min_mps")) {
        p.speedMinMps = obj["speed_min_mps"].asNumber(p.speedMinMps);
    }
    if (obj.contains("speed_off_mps")) {
        p.speedOffMps = obj["speed_off_mps"].asNumber(p.speedOffMps);
    }
    if (obj.contains("min_moving_s")) {
        p.minMovingS = obj["min_moving_s"].asNumber(p.minMovingS);
    }
    if (obj.contains("merge_gap_s")) {
        p.mergeGapS = obj["merge_gap_s"].asNumber(p.mergeGapS);
    }
    if (obj.contains("pad_s")) {
        p.padS = obj["pad_s"].asNumber(p.padS);
    }
    if (obj.contains("hole_s")) {
        p.holeS = obj["hole_s"].asNumber(p.holeS);
    }
    if (obj.contains("tz_offset_min")) {
        p.tzOffsetMin = static_cast<int>(obj["tz_offset_min"].asInt(p.tzOffsetMin));
    }
    return p;
}

json::Value paramsJson(const DetectorParams& p) {
    json::Object o;
    o.emplace("speed_min_mps", json::Value(p.speedMinMps));
    o.emplace("speed_off_mps", json::Value(p.speedOffMps));
    o.emplace("min_moving_s", json::Value(p.minMovingS));
    o.emplace("merge_gap_s", json::Value(p.mergeGapS));
    o.emplace("pad_s", json::Value(p.padS));
    o.emplace("hole_s", json::Value(p.holeS));
    o.emplace("tz_offset_min", json::Value(p.tzOffsetMin));
    return json::Value(std::move(o));
}

Session readSession(const json::Value& obj) {
    Session s;
    if (!obj.isObject()) {
        return s;
    }
    s.id = obj["id"].asString();
    s.date = obj["date"].asString();
    s.startMs = static_cast<std::uint64_t>(obj["start_ms"].asInt());
    s.endMs = static_cast<std::uint64_t>(obj["end_ms"].asInt());
    s.durationS = obj["duration_s"].asNumber();
    s.speedMaxMps = obj["speed_max_mps"].asNumber();
    s.speedMeanMps = obj["speed_mean_mps"].asNumber();
    s.speedSamples = static_cast<int>(obj["speed_samples"].asInt());
    s.file = obj["file"].asString();
    if (obj.contains("gps") && obj["gps"].isObject()) {
        const json::Value& g = obj["gps"];
        s.haveGps = true;
        s.gpsLat = g["lat"].asNumber();
        s.gpsLon = g["lon"].asNumber();
        s.gpsLatMin = g["lat_min"].asNumber(s.gpsLat);
        s.gpsLatMax = g["lat_max"].asNumber(s.gpsLat);
        s.gpsLonMin = g["lon_min"].asNumber(s.gpsLon);
        s.gpsLonMax = g["lon_max"].asNumber(s.gpsLon);
    }
    return s;
}

json::Value sessionJson(const Session& s) {
    json::Object o;
    o.emplace("id", json::Value(s.id));
    o.emplace("date", json::Value(s.date));
    o.emplace("start_ms", json::Value(s.startMs));
    o.emplace("end_ms", json::Value(s.endMs));
    o.emplace("duration_s", json::Value(s.durationS));
    o.emplace("speed_max_mps", json::Value(s.speedMaxMps));
    o.emplace("speed_mean_mps", json::Value(s.speedMeanMps));
    o.emplace("speed_samples", json::Value(s.speedSamples));
    if (!s.file.empty()) {
        o.emplace("file", json::Value(s.file));
    }
    if (s.haveGps) {
        json::Object g;
        g.emplace("lat", json::Value(s.gpsLat));
        g.emplace("lon", json::Value(s.gpsLon));
        g.emplace("lat_min", json::Value(s.gpsLatMin));
        g.emplace("lat_max", json::Value(s.gpsLatMax));
        g.emplace("lon_min", json::Value(s.gpsLonMin));
        g.emplace("lon_max", json::Value(s.gpsLonMax));
        o.emplace("gps", json::Value(std::move(g)));
    }
    return json::Value(std::move(o));
}

}  // namespace

std::string dateOf(std::uint64_t timestampMs, int tzOffsetMin) {
    const std::int64_t offsetMs = static_cast<std::int64_t>(tzOffsetMin) * 60000;
    const std::int64_t localMs = static_cast<std::int64_t>(timestampMs) + offsetMs;
    std::int64_t days = localMs / kMsPerDay;
    if (localMs < 0 && (localMs % kMsPerDay) != 0) {
        --days;
    }
    int year = 1970;
    while (true) {
        const int diy = isLeap(year) ? 366 : 365;
        if (days < diy) {
            break;
        }
        days -= diy;
        ++year;
    }
    const int leapDay = isLeap(year) ? 1 : 0;
    int month = 1;
    for (; month <= 12; ++month) {
        int dim = 31;
        if (month == 2) {
            dim = 28 + leapDay;
        } else if (month == 4 || month == 6 || month == 9 || month == 11) {
            dim = 30;
        }
        if (days < dim) {
            break;
        }
        days -= dim;
    }
    return formatDate(year, month, static_cast<int>(days) + 1);
}

std::vector<Session> detectSessions(const std::vector<SpeedSample>& speeds, const std::vector<GpsSample>& gps,
                                    const DetectorParams& params) {
    std::vector<SpeedSample> ordered = speeds;
    std::sort(ordered.begin(), ordered.end(),
              [](const SpeedSample& a, const SpeedSample& b) { return a.timestampMs < b.timestampMs; });

    struct RawRun {
        std::uint64_t startMs = 0;
        std::uint64_t endMs = 0;
    };
    std::vector<RawRun> runs;
    bool inRun = false;
    RawRun current{};
    std::uint64_t lastMovingMs = 0;
    const auto holeMs = static_cast<std::uint64_t>(std::max(0.0, params.holeS) * 1000.0);
    const auto minMs = static_cast<std::uint64_t>(std::max(0.0, params.minMovingS) * 1000.0);
    const auto mergeMs = static_cast<std::uint64_t>(std::max(0.0, params.mergeGapS) * 1000.0);
    const auto padMs = static_cast<std::uint64_t>(std::max(0.0, params.padS) * 1000.0);
    const double enterMps = params.speedMinMps;
    double leaveMps = params.speedOffMps;
    if (leaveMps <= 0.0 || leaveMps > enterMps) {
        leaveMps = enterMps;
    }

    auto closeRun = [&]() {
        if (!inRun) {
            return;
        }
        if (current.endMs >= current.startMs && (current.endMs - current.startMs) >= minMs) {
            runs.push_back(current);
        }
        inRun = false;
    };

    for (const auto& s : ordered) {
        const bool moving = s.speedMps >= (inRun ? leaveMps : enterMps);
        if (moving) {
            if (!inRun) {
                current.startMs = s.timestampMs;
                current.endMs = s.timestampMs;
                inRun = true;
            } else if (s.timestampMs > lastMovingMs + holeMs) {
                closeRun();
                current.startMs = s.timestampMs;
                current.endMs = s.timestampMs;
                inRun = true;
            } else {
                current.endMs = s.timestampMs;
            }
            lastMovingMs = s.timestampMs;
            continue;
        }
        if (inRun && s.timestampMs > lastMovingMs + holeMs) {
            closeRun();
        }
    }
    closeRun();

    std::vector<RawRun> merged;
    for (const auto& run : runs) {
        if (!merged.empty() && run.startMs <= merged.back().endMs + mergeMs) {
            merged.back().endMs = std::max(merged.back().endMs, run.endMs);
        } else {
            merged.push_back(run);
        }
    }

    std::vector<Session> sessions;
    sessions.reserve(merged.size());
    for (const auto& run : merged) {
        Session session;
        session.startMs = clampSub(run.startMs, padMs);
        session.endMs = run.endMs + padMs;
        session.durationS = static_cast<double>(session.endMs - session.startMs) / 1000.0;
        session.date = dateOf(session.startMs, params.tzOffsetMin);
        attachSpeedStats(session, ordered, run.startMs, run.endMs);
        attachGps(session, gps);
        sessions.push_back(std::move(session));
    }

    std::string prevDate;
    int index = 0;
    for (auto& session : sessions) {
        if (session.date != prevDate) {
            prevDate = session.date;
            index = 1;
        }
        session.id = sessionId(session.date, index);
        char file[64];
        std::snprintf(file, sizeof(file), "days/%s/session-%02d.log", session.date.c_str(), index);
        session.file = file;
        ++index;
    }
    return sessions;
}

Catalog catalogFromSessions(std::vector<Session> sessions, DetectorParams params, std::string source,
                            std::string timezone) {
    Catalog catalog;
    catalog.version = 1;
    catalog.source = std::move(source);
    catalog.timezone = std::move(timezone);
    catalog.params = params;

    std::sort(sessions.begin(), sessions.end(),
              [](const Session& a, const Session& b) { return a.startMs < b.startMs; });

    for (auto& session : sessions) {
        if (catalog.days.empty() || catalog.days.back().date != session.date) {
            Day day;
            day.date = session.date;
            day.path = "days/" + session.date;
            catalog.days.push_back(std::move(day));
        }
        Day& day = catalog.days.back();
        day.durationS += session.durationS;
        day.sessions.push_back(std::move(session));
    }
    return catalog;
}

bool timestampInSessions(std::uint64_t timestampMs, const std::vector<Session>& sessions) {
    if (sessions.empty()) {
        return false;
    }
    auto it = std::upper_bound(sessions.begin(), sessions.end(), timestampMs,
                               [](std::uint64_t ts, const Session& s) { return ts < s.startMs; });
    if (it != sessions.begin()) {
        --it;
        if (timestampMs <= it->endMs) {
            return true;
        }
    }
    return false;
}

json::Value toJson(const Catalog& catalog) {
    json::Object root;
    root.emplace("version", json::Value(catalog.version));
    root.emplace("source", json::Value(catalog.source));
    root.emplace("timezone", json::Value(catalog.timezone));
    root.emplace("detector", paramsJson(catalog.params));

    json::Array days;
    days.reserve(catalog.days.size());
    for (const auto& day : catalog.days) {
        json::Object d;
        d.emplace("date", json::Value(day.date));
        d.emplace("path", json::Value(day.path));
        d.emplace("session_count", json::Value(static_cast<int>(day.sessions.size())));
        d.emplace("duration_s", json::Value(day.durationS));
        json::Array sess;
        sess.reserve(day.sessions.size());
        for (const auto& s : day.sessions) {
            sess.emplace_back(sessionJson(s));
        }
        d.emplace("sessions", json::Value(std::move(sess)));
        days.emplace_back(std::move(d));
    }
    root.emplace("days", json::Value(std::move(days)));
    return json::Value(std::move(root));
}

std::string stringify(const Catalog& catalog) {
    return json::stringify(toJson(catalog));
}

std::optional<Catalog> parseCatalog(std::string_view text, std::string* error) {
    json::ParseError parseError;
    auto value = json::parse(text, &parseError);
    if (!value || !value->isObject()) {
        if (error) {
            *error = parseError.message.empty() ? "catalog is not a JSON object" : parseError.message;
        }
        return std::nullopt;
    }
    Catalog catalog;
    catalog.version = static_cast<int>((*value)["version"].asInt(1));
    catalog.source = (*value)["source"].asString();
    catalog.timezone = (*value)["timezone"].asString("America/Chicago");
    if ((*value).contains("detector")) {
        catalog.params = readParams((*value)["detector"]);
    }
    if (!(*value)["days"].isArray()) {
        if (error) {
            *error = "catalog missing days[]";
        }
        return std::nullopt;
    }
    for (const auto& dayVal : (*value)["days"].asArray()) {
        Day day;
        day.date = dayVal["date"].asString();
        day.path = dayVal["path"].asString();
        day.durationS = dayVal["duration_s"].asNumber();
        if (dayVal["sessions"].isArray()) {
            for (const auto& sessVal : dayVal["sessions"].asArray()) {
                day.sessions.push_back(readSession(sessVal));
            }
        }
        catalog.days.push_back(std::move(day));
    }
    return catalog;
}

std::optional<std::string> discoverCatalogPath(const std::string& path) {
    const fs::path root(path);
    const fs::path candidates[] = {
        root,
        root / "catalog.json",
        root / "on_track" / "catalog.json",
        root.parent_path() / "catalog.json",
        root.parent_path() / "on_track" / "catalog.json",
    };
    std::error_code ec;
    if (fs::is_regular_file(root, ec) && root.filename() == "catalog.json") {
        return root.string();
    }
    for (const auto& cand : candidates) {
        if (fs::is_regular_file(cand, ec) && cand.filename() == "catalog.json") {
            return cand.string();
        }
    }
    return std::nullopt;
}

namespace {

std::string absString(const fs::path& path) {
    std::error_code ec;
    const auto abs = fs::absolute(path, ec);
    return ec ? path.string() : abs.string();
}

bool isLogFile(const fs::path& path) {
    auto ext = path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".log";
}

std::optional<std::string> readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string resolveDayDir(const fs::path& catalogFile, const Day& day) {
    const auto root = catalogFile.parent_path();
    std::vector<fs::path> candidates;
    if (!day.path.empty()) {
        candidates.push_back(root / day.path);
    }
    if (!day.date.empty()) {
        candidates.push_back(root / "days" / day.date);
    }
    if (!day.sessions.empty() && !day.sessions.front().file.empty()) {
        candidates.push_back((root / day.sessions.front().file).parent_path());
    }
    for (const auto& cand : candidates) {
        if (directoryHasLogs(cand.string())) {
            return absString(cand);
        }
    }
    if (!day.date.empty()) {
        return absString(root / "days" / day.date);
    }
    return absString(root);
}

std::vector<DriveDay> daysFromCatalogFile(const std::string& catalogPath) {
    std::vector<DriveDay> out;
    const auto text = readTextFile(catalogPath);
    if (!text) {
        return out;
    }
    auto catalog = parseCatalog(*text);
    if (!catalog) {
        return out;
    }
    const auto catalogRoot = fs::path(catalogPath).parent_path();
    for (const auto& day : catalog->days) {
        DriveDay item;
        item.date = day.date;
        item.path = resolveDayDir(catalogPath, day);
        item.sessionCount = static_cast<int>(day.sessions.size());
        item.durationS = day.durationS;
        for (const auto& sess : day.sessions) {
            DriveSession row;
            row.id = sess.id.empty() ? sess.file : sess.id;
            if (!sess.file.empty()) {
                const fs::path filePath = fs::path(sess.file).is_absolute() ? fs::path(sess.file)
                                                                           : catalogRoot / sess.file;
                if (directoryHasLogs(filePath.string())) {
                    row.path = absString(filePath);
                }
            }
            if (row.path.empty() && directoryHasLogs(item.path)) {
                row.path = item.path;
            }
            row.startMs = sess.startMs;
            row.durationS = sess.durationS;
            row.speedMaxMps = sess.speedMaxMps;
            item.sessions.push_back(std::move(row));
        }
        if (item.sessionCount == 0 && directoryHasLogs(item.path)) {
            item.sessionCount = 1;
        }
        if (!directoryHasLogs(item.path)) {
            continue;
        }
        out.push_back(std::move(item));
    }
    std::sort(out.begin(), out.end(),
              [](const DriveDay& a, const DriveDay& b) { return a.date < b.date; });
    return out;
}

std::vector<std::string> extraCatalogRoots() {
    std::vector<std::string> roots;
    if (const char* env = std::getenv("PHOTON_ON_TRACK")) {
        if (env[0] != '\0') {
            roots.emplace_back(env);
        }
    }
    roots.emplace_back(R"(F:\lhr\_logs_extract\on_track)");
    std::error_code ec;
    const auto cwd = fs::current_path(ec);
    if (!ec) {
        roots.push_back((cwd / "_logs_extract" / "on_track").string());
        roots.push_back((cwd.parent_path() / "_logs_extract" / "on_track").string());
        roots.push_back((cwd / "on_track").string());
    }
    return roots;
}

}  // namespace

bool directoryHasLogs(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const fs::path root(path);
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        return isLogFile(root);
    }
    if (!fs::is_directory(root, ec)) {
        return false;
    }
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec) && isLogFile(entry.path())) {
            return true;
        }
    }
    return false;
}

std::string localYmd(std::uint64_t ms) {
    if (ms < 100000000000ull) {
        return {};
    }
    const std::time_t sec = static_cast<std::time_t>(ms / 1000ull);
    std::tm tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &sec) != 0) {
        return {};
    }
#else
    if (localtime_r(&sec, &tm) == nullptr) {
        return {};
    }
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

// Live CAN segments are one VSD1 log spanning several days. Split that span
// into one replay day each, beside the on-track catalog.
void appendSegmentDays(std::vector<DriveDay>& days) {
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("PHOTON_DATA_DIR"); env != nullptr && env[0] != '\0') {
        roots.emplace_back(fs::path(env) / "segments");
    }
    roots.emplace_back(R"(F:\lhr\data\segments)");
    std::unordered_set<std::string> have;
    for (const auto& day : days) {
        have.insert(day.date);
    }
    std::unordered_set<std::string> seenRoot;
    for (const auto& root : roots) {
        std::error_code ec;
        const auto canon = fs::weakly_canonical(root, ec);
        const std::string key = ec ? root.string() : canon.string();
        if (!seenRoot.insert(key).second || !directoryHasLogs(key)) {
            continue;
        }
        const auto files = vsd1::listLogFiles(key);
        if (files.empty()) {
            continue;
        }
        struct Span {
            std::uint64_t minMs = 0;
            std::uint64_t maxMs = 0;
        };
        std::unordered_map<std::string, Span> spans;
        std::string error;
        vsd1::walkLogFiles(files, nullptr, &error, [&](vsd1::IndexedBatch&& item) {
            const auto ymd = localYmd(item.timestampMs);
            if (ymd.empty() || have.count(ymd) != 0) {
                return;
            }
            auto& span = spans[ymd];
            if (span.minMs == 0 || item.timestampMs < span.minMs) {
                span.minMs = item.timestampMs;
            }
            if (item.timestampMs > span.maxMs) {
                span.maxMs = item.timestampMs;
            }
        });
        for (const auto& [ymd, span] : spans) {
            if (span.maxMs <= span.minMs) {
                continue;
            }
            DriveDay day;
            day.date = ymd;
            day.path = key;
            day.sessionCount = 1;
            day.durationS = static_cast<double>(span.maxMs - span.minMs) / 1000.0;
            DriveSession sess;
            sess.id = "segments-" + ymd;
            sess.path = key;
            sess.startMs = span.minMs;
            sess.durationS = day.durationS;
            day.sessions.push_back(std::move(sess));
            days.push_back(std::move(day));
            have.insert(ymd);
        }
    }
    std::sort(days.begin(), days.end(),
              [](const DriveDay& a, const DriveDay& b) { return a.date < b.date; });
}

bool includeLiveSegments(const std::string& hint) {
    if (hint.empty()) {
        return true;
    }
    return hint.find("on_track") != std::string::npos || hint.find("_logs_extract") != std::string::npos;
}

std::vector<DriveDay> listDriveDays(const std::string& hintPath) {
    std::vector<DriveDay> out;
    if (!hintPath.empty()) {
        if (const auto catalog = discoverCatalogPath(hintPath)) {
            out = daysFromCatalogFile(*catalog);
        }
    }
    if (out.empty()) {
        std::unordered_set<std::string> seen;
        for (const auto& root : extraCatalogRoots()) {
            const auto catalog = discoverCatalogPath(root);
            if (!catalog || !seen.insert(*catalog).second) {
                continue;
            }
            out = daysFromCatalogFile(*catalog);
            if (!out.empty()) {
                break;
            }
        }
    }
    if (includeLiveSegments(hintPath)) {
        appendSegmentDays(out);
    }
    return out;
}

std::string resolveReplayPath(const std::string& path) {
    if (directoryHasLogs(path)) {
        return absString(path);
    }
    const auto days = listDriveDays(path);
    if (days.empty()) {
        return path;
    }
    if (!path.empty()) {
        for (const auto& day : days) {
            if (path.find(day.date) != std::string::npos) {
                return day.path;
            }
        }
    }
    const auto best = std::max_element(days.begin(), days.end(),
                                       [](const DriveDay& a, const DriveDay& b) {
                                           return a.durationS < b.durationS;
                                       });
    return best->path;
}

}  // namespace photon::logsess
