#include "export.hpp"
#include "parquet.hpp"
#include "photon/tsdb.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;

namespace {

std::uint64_t peakWorkingSetBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) != 0) {
        return static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
    }
    return 0;
#else
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#if defined(__APPLE__)
        return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
        return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024ull;
#endif
    }
    return 0;
#endif
}

void usage(const char* argv0) {
    std::fprintf(stdout,
                 "Usage: %s --store DIR --session ID --out FILE (--signals LIST | --all-signals)\n"
                 "       %s --store DIR --list-sessions\n"
                 "       %s --store DIR --session ID --list-signals\n"
                 "  Export a time range of TSDB signals to Parquet for pandas/DuckDB/Spark.\n"
                 "\n"
                 "  --store DIR         TSDB root (default: $PHOTON_TSDB, else data/tsdb)\n"
                 "  --session ID        session directory under --store\n"
                 "  --out FILE          output .parquet path; a <stem>.signals.parquet sidecar\n"
                 "                      carrying the signal dictionary is written beside it\n"
                 "  --signals LIST      comma-separated signal ids and/or Message.Signal names\n"
                 "  --all-signals       every signal that has data in this session\n"
                 "  --from MS           window start, unix ms (default: session start)\n"
                 "  --to MS             window end, unix ms (default: session end)\n"
                 "  --level N           force resolution level 0..5, degrading to the finest\n"
                 "                      level built for a signal (default: auto per signal,\n"
                 "                      chosen the way the query engine picks)\n"
                 "  --max-points N      point budget driving auto level selection (default 2000)\n"
                 "  --row-group-rows N  rows buffered per Parquet row group (default 262144)\n"
                 "  --page-rows N       values per data page (default 32768)\n"
                 "  --no-stats          omit column-chunk statistics\n"
                 "  --probe MS          instead of exporting, print the raw samples the query\n"
                 "                      engine returns for [MS-W, MS+W] as TSV, for checking an\n"
                 "                      exported file against Store::query()\n"
                 "  --probe-window MS   half-width for --probe (default 500)\n"
                 "\n"
                 "  Range is inclusive on both ends. Rollup buckets are included when their\n"
                 "  slot interval intersects the window, so edge buckets may extend past it.\n",
                 argv0, argv0, argv0);
}

std::vector<std::string> splitCommas(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto comma = s.find(',', start);
        auto piece = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) {
            piece.erase(piece.begin());
        }
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t')) {
            piece.pop_back();
        }
        if (!piece.empty()) {
            out.push_back(std::move(piece));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

// Long-format column layout. One row per exported point; the envelope columns
// are null on raw rows and the value/raw pair is null on rollup rows, and PLAIN
// encoding stores nothing for a null, so neither shape pays for the other.
enum Col : std::size_t {
    kSignalId = 0,
    kTMs,
    kLevel,
    kBucketMs,
    kValue,
    kRaw,
    kVMin,
    kVMax,
    kVMean,
    kVFirst,
    kVLast,
    kCount,
    kColumnCount,
};

std::vector<photon::pq::ColumnSpec> dataSchema() {
    using photon::pq::ColumnSpec;
    using photon::pq::Logical;
    using photon::pq::Type;
    return {
        ColumnSpec{"signal_id", Type::Int32, Logical::Uint32, false},
        ColumnSpec{"t_ms", Type::Int64, Logical::TimestampMillisUtc, false},
        ColumnSpec{"level", Type::Int32, Logical::None, false},
        ColumnSpec{"bucket_ms", Type::Int32, Logical::None, false},
        ColumnSpec{"value", Type::Double, Logical::None, true},
        ColumnSpec{"raw", Type::Int64, Logical::None, true},
        ColumnSpec{"v_min", Type::Double, Logical::None, true},
        ColumnSpec{"v_max", Type::Double, Logical::None, true},
        ColumnSpec{"v_mean", Type::Double, Logical::None, true},
        ColumnSpec{"v_first", Type::Double, Logical::None, true},
        ColumnSpec{"v_last", Type::Double, Logical::None, true},
        ColumnSpec{"count", Type::Int64, Logical::None, true},
    };
}

class ParquetSink final : public photon::pexport::RowSink {
public:
    explicit ParquetSink(photon::pq::Writer& w) : w_(w) {}

    bool row(const photon::pexport::ExportRow& r) override {
        w_.setInt32(kSignalId, static_cast<std::int32_t>(r.signalId));
        w_.setInt64(kTMs, static_cast<std::int64_t>(r.tMs));
        w_.setInt32(kLevel, r.level);
        w_.setInt32(kBucketMs, static_cast<std::int32_t>(r.bucketMs));
        if (r.hasValue) {
            w_.setDouble(kValue, r.value);
            w_.setInt64(kRaw, r.raw);
        } else {
            w_.setNull(kValue);
            w_.setNull(kRaw);
        }
        if (r.hasEnvelope && r.hasMinMax) {
            w_.setDouble(kVMin, r.vMin);
            w_.setDouble(kVMax, r.vMax);
        } else {
            w_.setNull(kVMin);
            w_.setNull(kVMax);
        }
        if (r.hasEnvelope && r.hasMean) {
            w_.setDouble(kVMean, r.vMean);
        } else {
            w_.setNull(kVMean);
        }
        if (r.hasEnvelope) {
            w_.setDouble(kVFirst, r.vFirst);
            w_.setDouble(kVLast, r.vLast);
            w_.setInt64(kCount, static_cast<std::int64_t>(r.count));
        } else {
            w_.setNull(kVFirst);
            w_.setNull(kVLast);
            w_.setNull(kCount);
        }
        return w_.endRow();
    }

private:
    photon::pq::Writer& w_;
};

bool writeSignalSidecar(const fs::path& path, const photon::tsdb::Store& store,
                        const std::vector<photon::pexport::SignalReport>& reports, std::string* error) {
    using photon::pq::ColumnSpec;
    using photon::pq::Logical;
    using photon::pq::Type;
    std::vector<ColumnSpec> schema{
        ColumnSpec{"signal_id", Type::Int32, Logical::Uint32, false},
        ColumnSpec{"message", Type::ByteArray, Logical::Utf8, false},
        ColumnSpec{"name", Type::ByteArray, Logical::Utf8, false},
        ColumnSpec{"unit", Type::ByteArray, Logical::Utf8, false},
        ColumnSpec{"factor", Type::Double, Logical::None, false},
        ColumnSpec{"offset", Type::Double, Logical::None, false},
        ColumnSpec{"bits", Type::Int32, Logical::None, false},
        ColumnSpec{"is_signed", Type::Boolean, Logical::None, false},
        ColumnSpec{"value_type", Type::Int32, Logical::None, false},
        ColumnSpec{"native_hz", Type::Double, Logical::None, false},
        ColumnSpec{"built_levels", Type::Int32, Logical::None, false},
        ColumnSpec{"export_level", Type::Int32, Logical::None, false},
        ColumnSpec{"rows", Type::Int64, Logical::None, false},
        ColumnSpec{"envelope_suppressed", Type::Boolean, Logical::None, false},
    };
    photon::pq::Writer w;
    photon::pq::WriterOptions opts;
    opts.rowGroupRows = 4096;
    opts.pageRows = 4096;
    if (!w.open(path, std::move(schema), opts, error)) {
        return false;
    }
    for (const auto& sr : reports) {
        const auto* info = store.signal(sr.signalId);
        w.setInt32(0, static_cast<std::int32_t>(sr.signalId));
        w.setString(1, info != nullptr ? info->message : std::string{});
        w.setString(2, info != nullptr ? info->name : std::string{});
        w.setString(3, info != nullptr ? info->unit : std::string{});
        w.setDouble(4, info != nullptr ? info->factor : 1.0);
        w.setDouble(5, info != nullptr ? info->offset : 0.0);
        w.setInt32(6, info != nullptr ? info->bits : 0);
        w.setBool(7, info != nullptr && info->isSigned);
        w.setInt32(8, info != nullptr ? static_cast<std::int32_t>(info->valueType) : 0);
        w.setDouble(9, info != nullptr ? info->nativeHz : 0.0);
        w.setInt32(10, static_cast<std::int32_t>(store.levelsOf(sr.signalId)));
        w.setInt32(11, sr.level);
        w.setInt64(12, static_cast<std::int64_t>(sr.rows));
        w.setBool(13, sr.envelopeSuppressed);
        if (!w.endRow(error)) {
            return false;
        }
    }
    return w.close(error);
}

std::string boundariesJson(const photon::tsdb::Store& store,
                           const std::vector<photon::pexport::SignalReport>& reports) {
    std::string out = "[";
    bool first = true;
    for (const auto& sr : reports) {
        if (!sr.prior && !sr.next) {
            continue;
        }
        const auto* info = store.signal(sr.signalId);
        if (!first) {
            out += ",";
        }
        first = false;
        out += "{\"signal_id\":" + std::to_string(sr.signalId);
        const auto emit = [&](const char* key, const std::optional<photon::tsdb::RawSample>& s) {
            if (!s) {
                return;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.17g",
                          info != nullptr ? photon::tsdb::physical(*info, s->raw)
                                          : static_cast<double>(s->raw));
            out += std::string(",\"") + key + "\":{\"t_ms\":" + std::to_string(s->timestampMs) +
                   ",\"raw\":" + std::to_string(s->raw) + ",\"value\":" + buf + "}";
        };
        emit("prior", sr.prior);
        emit("next", sr.next);
        out += "}";
    }
    out += "]";
    return out;
}

int listSessions(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (entry.is_directory() && fs::exists(entry.path() / "session.json")) {
            dirs.push_back(entry.path());
        }
    }
    if (dirs.empty()) {
        std::fprintf(stderr, "[photon-export] no sessions under %s\n", root.string().c_str());
        return 1;
    }
    std::sort(dirs.begin(), dirs.end());
    std::fprintf(stdout, "[photon-export] %zu session(s) under %s\n", dirs.size(), root.string().c_str());
    for (const auto& dir : dirs) {
        photon::tsdb::Store s;
        if (!s.open(dir)) {
            continue;
        }
        const auto& m = s.manifest();
        std::fprintf(stdout, "[photon-export] %s from=%llu to=%llu span=%.1f s samples=%llu levels=0x%02x\n",
                     m.id.c_str(), static_cast<unsigned long long>(m.minMs),
                     static_cast<unsigned long long>(m.maxMs),
                     static_cast<double>(m.maxMs - m.minMs) / 1000.0,
                     static_cast<unsigned long long>(m.samples), static_cast<unsigned>(m.builtLevels));
    }
    return 0;
}

std::vector<std::uint32_t> signalsWithData(const photon::tsdb::Store& store) {
    std::set<std::uint32_t> ids;
    for (const auto& rec : store.index(0)) {
        ids.insert(rec.signalId);
    }
    return {ids.begin(), ids.end()};
}

int listSignals(const photon::tsdb::Store& store) {
    const auto ids = signalsWithData(store);
    std::fprintf(stdout, "[photon-export] %zu signal(s) with data\n", ids.size());
    std::fprintf(stdout, "[photon-export] %-12s %-38s %8s %6s %s\n", "id", "message.signal", "native_hz",
                 "levels", "unit");
    for (const auto id : ids) {
        const auto* info = store.signal(id);
        const auto label =
            info != nullptr ? info->message + "." + info->name : std::string("<unknown>");
        std::fprintf(stdout, "[photon-export] %-12u %-38s %8.3f   0x%02x %s\n", id, label.c_str(),
                     info != nullptr ? info->nativeHz : 0.0, static_cast<unsigned>(store.levelsOf(id)),
                     info != nullptr ? info->unit.c_str() : "");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string storeArg;
    std::string session;
    std::string outArg;
    std::string signalsArg;
    bool allSignals = false;
    bool doListSessions = false;
    bool doListSignals = false;
    bool haveFrom = false;
    bool haveTo = false;
    std::uint64_t fromMs = 0;
    std::uint64_t toMs = 0;
    int level = -1;
    std::uint32_t maxPoints = 2000;
    bool haveProbe = false;
    std::uint64_t probeMs = 0;
    std::uint64_t probeWindowMs = 500;
    photon::pq::WriterOptions writerOpts;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](std::string& dest) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[photon-export] %s requires a value\n", arg.c_str());
                return false;
            }
            dest = argv[++i];
            return true;
        };
        auto needU64 = [&](std::uint64_t& dest) -> bool {
            std::string value;
            if (!need(value)) {
                return false;
            }
            errno = 0;
            char* end = nullptr;
            const auto n = std::strtoull(value.c_str(), &end, 10);
            if (end == value.c_str() || (end != nullptr && *end != '\0') || errno != 0) {
                std::fprintf(stderr, "[photon-export] %s needs a non-negative integer\n", arg.c_str());
                return false;
            }
            dest = n;
            return true;
        };
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (arg == "--store") {
            if (!need(storeArg)) {
                return 2;
            }
        } else if (arg == "--session") {
            if (!need(session)) {
                return 2;
            }
        } else if (arg == "--out") {
            if (!need(outArg)) {
                return 2;
            }
        } else if (arg == "--signals") {
            if (!need(signalsArg)) {
                return 2;
            }
        } else if (arg == "--all-signals") {
            allSignals = true;
        } else if (arg == "--list-sessions") {
            doListSessions = true;
        } else if (arg == "--list-signals") {
            doListSignals = true;
        } else if (arg == "--no-stats") {
            writerOpts.statistics = false;
        } else if (arg == "--from") {
            if (!needU64(fromMs)) {
                return 2;
            }
            haveFrom = true;
        } else if (arg == "--to") {
            if (!needU64(toMs)) {
                return 2;
            }
            haveTo = true;
        } else if (arg == "--probe") {
            if (!needU64(probeMs)) {
                return 2;
            }
            haveProbe = true;
        } else if (arg == "--probe-window") {
            if (!needU64(probeWindowMs)) {
                return 2;
            }
        } else if (arg == "--level") {
            std::uint64_t n = 0;
            if (!needU64(n)) {
                return 2;
            }
            if (n >= static_cast<std::uint64_t>(photon::tsdb::kLevelCount)) {
                std::fprintf(stderr, "[photon-export] --level must be 0..%d\n",
                             photon::tsdb::kLevelCount - 1);
                return 2;
            }
            level = static_cast<int>(n);
        } else if (arg == "--max-points" || arg == "--row-group-rows" || arg == "--page-rows") {
            std::uint64_t n = 0;
            if (!needU64(n)) {
                return 2;
            }
            if (n == 0 || n > 0xFFFFFFFFull) {
                std::fprintf(stderr, "[photon-export] %s needs a positive 32-bit value\n", arg.c_str());
                return 2;
            }
            if (arg == "--max-points") {
                maxPoints = static_cast<std::uint32_t>(n);
            } else if (arg == "--row-group-rows") {
                writerOpts.rowGroupRows = static_cast<std::uint32_t>(n);
            } else {
                writerOpts.pageRows = static_cast<std::uint32_t>(n);
            }
        } else {
            std::fprintf(stderr, "[photon-export] unknown flag %s\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (storeArg.empty()) {
#if defined(_MSC_VER) || defined(_WIN32)
        char buf[512];
        std::size_t n = 0;
        storeArg = getenv_s(&n, buf, sizeof(buf), "PHOTON_TSDB") == 0 && n > 1 ? std::string(buf)
                                                                              : std::string("data/tsdb");
#else
        const char* env = std::getenv("PHOTON_TSDB");
        storeArg = env != nullptr ? std::string(env) : std::string("data/tsdb");
#endif
    }
    const fs::path storeRoot(storeArg);
    if (doListSessions) {
        return listSessions(storeRoot);
    }
    if (session.empty()) {
        usage(argv[0]);
        return 2;
    }

    photon::tsdb::Store store;
    std::string error;
    if (!store.open(storeRoot / session, &error)) {
        std::fprintf(stderr, "[photon-export] cannot open session %s: %s\n", session.c_str(), error.c_str());
        return 1;
    }
    const auto& man = store.manifest();
    if (doListSignals) {
        return listSignals(store);
    }
    if (outArg.empty() && !haveProbe) {
        usage(argv[0]);
        return 2;
    }

    std::vector<std::uint32_t> ids;
    if (allSignals) {
        ids = signalsWithData(store);
    }
    if (!signalsArg.empty()) {
        for (const auto& token : splitCommas(signalsArg)) {
            bool numeric = !token.empty();
            for (const char c : token) {
                if (c < '0' || c > '9') {
                    numeric = false;
                    break;
                }
            }
            if (numeric) {
                ids.push_back(static_cast<std::uint32_t>(std::strtoull(token.c_str(), nullptr, 10)));
                continue;
            }
            bool found = false;
            std::uint32_t matched = 0;
            for (const auto& [id, info] : store.signals()) {
                if (token == info.message + "." + info.name || token == info.name) {
                    matched = id;
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::fprintf(stderr, "[photon-export] no signal matches '%s' in %s\n", token.c_str(),
                             session.c_str());
                return 1;
            }
            ids.push_back(matched);
        }
    }
    if (ids.empty()) {
        std::fprintf(stderr,
                     "[photon-export] no signals selected; pass --signals LIST or --all-signals\n");
        return 2;
    }
    // Stable and duplicate-free, so the row table is sorted by (signal_id, t_ms)
    // and a signal named twice is not exported twice.
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    if (!haveFrom) {
        fromMs = man.minMs;
    }
    if (!haveTo) {
        toMs = man.maxMs;
    }
    if (fromMs > toMs) {
        std::fprintf(stderr, "[photon-export] --from %llu is after --to %llu\n",
                     static_cast<unsigned long long>(fromMs), static_cast<unsigned long long>(toMs));
        return 2;
    }

    if (haveProbe) {
        // Deliberately goes through Store::query() rather than the page scan the
        // export uses, so comparing the two checks the exporter against the
        // query engine instead of against itself.
        photon::tsdb::Query q;
        q.fromMs = probeMs > probeWindowMs ? probeMs - probeWindowMs : 0;
        q.toMs = probeMs + probeWindowMs;
        q.signalIds = ids;
        // Big enough that level selection lands on L0 for any sane window.
        q.maxPoints = 1u << 30;
        const auto res = store.query(q);
        std::fprintf(stdout, "signal_id\tt_ms\traw\tvalue\tlevel\n");
        for (const auto& s : res.series) {
            const auto* info = store.signal(s.signalId);
            for (std::size_t k = 0; k < s.timestamps.size() && k < s.raw.size(); ++k) {
                std::fprintf(stdout, "%u\t%llu\t%lld\t%.17g\t%d\n", s.signalId,
                             static_cast<unsigned long long>(s.timestamps[k]),
                             static_cast<long long>(s.raw[k]),
                             info != nullptr ? photon::tsdb::physical(*info, s.raw[k])
                                             : static_cast<double>(s.raw[k]),
                             s.level);
            }
        }
        return 0;
    }

    const fs::path outPath(outArg);
    fs::path sidecarPath = outPath;
    sidecarPath.replace_extension();
    sidecarPath += ".signals.parquet";

    std::fprintf(stdout, "[photon-export] store %s session %s\n", storeRoot.string().c_str(),
                 man.id.c_str());
    std::fprintf(stdout, "[photon-export] dbc %s (%s)\n", man.dbcLabel.c_str(), man.dbcSha.c_str());
    std::fprintf(stdout,
                 "[photon-export] window [%llu, %llu] = %.3f s, %zu signal(s), level=%s, max_points=%u\n",
                 static_cast<unsigned long long>(fromMs), static_cast<unsigned long long>(toMs),
                 static_cast<double>(toMs - fromMs) / 1000.0, ids.size(),
                 level < 0 ? "auto" : ("L" + std::to_string(level)).c_str(), maxPoints);
    std::fprintf(stdout, "[photon-export] out %s (+ %s)\n", outPath.string().c_str(),
                 sidecarPath.filename().string().c_str());

    photon::pq::Writer writer;
    if (!writer.open(outPath, dataSchema(), writerOpts, &error)) {
        std::fprintf(stderr, "[photon-export] %s\n", error.c_str());
        return 1;
    }
    writer.setMetadata("photon.schema_version", "1");
    writer.setMetadata("photon.session_id", man.id);
    writer.setMetadata("photon.dbc_sha", man.dbcSha);
    writer.setMetadata("photon.dbc_label", man.dbcLabel);
    writer.setMetadata("photon.from_ms", std::to_string(fromMs));
    writer.setMetadata("photon.to_ms", std::to_string(toMs));
    writer.setMetadata("photon.max_points", std::to_string(maxPoints));
    writer.setMetadata("photon.level_policy", level < 0 ? "auto" : "forced:L" + std::to_string(level));
    writer.setMetadata("photon.signals_file", sidecarPath.filename().string());

    photon::pexport::Request req;
    req.fromMs = fromMs;
    req.toMs = toMs;
    req.signalIds = ids;
    req.level = level;
    req.maxPoints = maxPoints;

    ParquetSink sink(writer);
    const auto t0 = std::chrono::steady_clock::now();
    auto report = photon::pexport::exportRange(store, req, sink);
    if (!report.ok) {
        std::fprintf(stderr, "[photon-export] export failed after %llu row(s): %s\n",
                     static_cast<unsigned long long>(report.rows),
                     report.error.empty() ? writer.error().c_str() : report.error.c_str());
        (void)writer.close(&error);
        return 1;
    }
    writer.setMetadata("photon.boundaries", boundariesJson(store, report.signals));
    if (!writer.close(&error)) {
        std::fprintf(stderr, "[photon-export] parquet close failed: %s\n", error.c_str());
        return 1;
    }
    if (!writeSignalSidecar(sidecarPath, store, report.signals, &error)) {
        std::fprintf(stderr, "[photon-export] sidecar write failed: %s\n", error.c_str());
        return 1;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedS = std::chrono::duration<double>(t1 - t0).count();

    std::uint64_t sidecarBytes = 0;
    std::error_code ec;
    sidecarBytes = static_cast<std::uint64_t>(fs::file_size(sidecarPath, ec));
    if (ec) {
        sidecarBytes = 0;
    }

    std::set<int> levelsUsed;
    std::uint32_t suppressed = 0;
    for (const auto& sr : report.signals) {
        levelsUsed.insert(sr.level);
        if (sr.envelopeSuppressed) {
            ++suppressed;
        }
        std::fprintf(stdout, "[photon-export] signal %-12u L%d rows=%llu pages=%llu pageBytes=%llu%s\n",
                     sr.signalId, sr.level, static_cast<unsigned long long>(sr.rows),
                     static_cast<unsigned long long>(sr.pages),
                     static_cast<unsigned long long>(sr.pageBytes),
                     sr.envelopeSuppressed ? "  (min/max/mean suppressed: float value type)" : "");
    }
    std::string levelText;
    for (const auto L : levelsUsed) {
        levelText += (levelText.empty() ? "L" : ",L") + std::to_string(L);
    }
    std::fprintf(stdout,
                 "[photon-export] total rows=%llu bytes=%llu (+%llu sidecar) row_groups=%u "
                 "levels=%s elapsed=%.3f s\n",
                 static_cast<unsigned long long>(writer.rows()),
                 static_cast<unsigned long long>(writer.bytes()),
                 static_cast<unsigned long long>(sidecarBytes), writer.rowGroups(),
                 levelText.empty() ? "-" : levelText.c_str(), elapsedS);
    std::fprintf(stdout, "[photon-export] store read pages=%llu bytes=%llu max_page=%llu B\n",
                 static_cast<unsigned long long>(report.pages),
                 static_cast<unsigned long long>(report.pageBytes),
                 static_cast<unsigned long long>(report.maxPageBytes));
    if (writer.rows() > 0) {
        std::fprintf(stdout, "[photon-export] %.2f B/row, %.0f rows/s, %.2f MB/s written\n",
                     static_cast<double>(writer.bytes()) / static_cast<double>(writer.rows()),
                     elapsedS > 0.0 ? static_cast<double>(writer.rows()) / elapsedS : 0.0,
                     elapsedS > 0.0 ? static_cast<double>(writer.bytes()) / elapsedS / (1024.0 * 1024.0)
                                    : 0.0);
    }
    if (suppressed > 0) {
        std::fprintf(stdout,
                     "[photon-export] %u signal(s) exported without min/max/mean: the store rolls up in "
                     "the raw integer domain and their DBC value type is Float/Double, so an integer "
                     "ordering of the bit patterns would invert negative values\n",
                     suppressed);
    }
    if (writer.rows() == 0) {
        std::fprintf(stdout, "[photon-export] no data in range; the file is a valid empty table\n");
    }
    std::fprintf(stdout, "[photon-export] peak working set %.1f MB\n",
                 static_cast<double>(peakWorkingSetBytes()) / (1024.0 * 1024.0));
    return 0;
}
