#pragma once

#include "photon/tsdb.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace photon::pexport {

// One exported point. At L0 the value/raw pair is set and the envelope is not;
// at L>=1 the reverse. Keeping both shapes in one row type is what lets a single
// long-format table carry an export whose level differs per signal, which the
// query engine's per-signal level choice (spec §6) makes the normal case.
struct ExportRow {
    std::uint32_t signalId = 0;
    // L0: sample timestamp. L>=1: bucket start, aligned to bucketMs.
    std::uint64_t tMs = 0;
    int level = 0;
    std::uint32_t bucketMs = 0;

    bool hasValue = false;
    double value = 0.0;
    std::int64_t raw = 0;

    bool hasEnvelope = false;
    // Suppressed for signals whose DBC value type is Float/Double: the store
    // rolls up in the raw integer domain, and ordering float bit patterns as
    // integers reverses the order of negative values, so min/max/mean would be
    // wrong rather than merely coarse.
    bool hasMinMax = false;
    double vMin = 0.0;
    double vMax = 0.0;
    bool hasMean = false;
    double vMean = 0.0;
    double vFirst = 0.0;
    double vLast = 0.0;
    std::uint64_t count = 0;
};

class RowSink {
public:
    virtual ~RowSink() = default;
    // Returning false aborts the export and is reported as an error.
    virtual bool row(const ExportRow& r) = 0;
};

struct Request {
    std::uint64_t fromMs = 0;
    std::uint64_t toMs = 0;
    std::vector<std::uint32_t> signalIds;
    // -1 means pick per signal the way the query engine does. 0..5 forces a
    // level, degrading to the next finer one where it was not built.
    int level = -1;
    std::uint32_t maxPoints = 2000;
};

struct SignalReport {
    std::uint32_t signalId = 0;
    int level = 0;
    std::uint64_t rows = 0;
    std::uint64_t pages = 0;
    std::uint64_t pageBytes = 0;
    bool envelopeSuppressed = false;
    std::optional<tsdb::RawSample> prior;
    std::optional<tsdb::RawSample> next;
};

struct Report {
    std::uint64_t rows = 0;
    std::uint64_t pages = 0;
    std::uint64_t pageBytes = 0;
    // Largest single page payload decoded. This is the exporter's per-page
    // working set, which is what keeps a 120 M sample session bounded.
    std::uint64_t maxPageBytes = 0;
    std::vector<SignalReport> signals;
    bool ok = false;
    std::string error;
};

// Level selection for one signal, using the same inputs as the query engine:
// in-range raw count, the point budget, and the levels built for that signal.
int autoLevel(const tsdb::Store& store, std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
              std::uint32_t maxPoints);

// Degrade a requested level to the finest built one at or below it.
int clampLevel(int requested, std::uint8_t builtLevels);

// Spec §6 range rules: raw samples inclusive on both ends, buckets included
// when their slot interval [start, start + bucketMs) intersects [from, to].
bool rawInRange(std::uint64_t tMs, std::uint64_t fromMs, std::uint64_t toMs);
bool bucketInRange(std::uint64_t startMs, std::uint32_t bucketMs, std::uint64_t fromMs, std::uint64_t toMs);

// Bucket mean in the raw integer domain. The persisted sum is one or two 64-bit
// words (spec §5.1); the two-word case only arises on long sessions of large
// values, so it is exposed here to be testable without such a session on disk.
double meanRaw(const tsdb::RollupBucket& b, bool isSigned);

// Streams one page at a time out of the level's page index, so peak memory is
// one page plus whatever the sink buffers.
Report exportRange(const tsdb::Store& store, const Request& req, RowSink& sink);

std::vector<std::uint32_t> signalsWithData(const tsdb::Store& store);

// Match "Message.Signal" exactly, or a unique signal name. Unknown selectors are
// skipped. Order follows the selector list; duplicates collapse.
std::vector<std::uint32_t> resolveSignalSelectors(const tsdb::Store& store,
                                                  const std::vector<std::string>& selectors);

// Long-format CSV: t_ms,message,signal,unit,value
bool writeCsv(const tsdb::Store& store, const Request& req,
              const std::function<bool(std::string_view)>& emit, Report* report);

struct CsvQuery {
    std::string session;
    std::uint64_t fromMs = 0;
    std::uint64_t toMs = 0;
    bool haveFrom = false;
    bool haveTo = false;
    // 1 Hz buckets. Raw L0 is opt-in because a half-hour run is about a gigabyte.
    int level = 2;
    bool allSignals = false;
    std::vector<std::string> selectors;
    std::string error;
};

bool validSessionId(std::string_view id);
bool parseCsvQuery(std::string_view target, CsvQuery* out);
void csvEscape(std::string* out, std::string_view field);

}  // namespace photon::pexport
