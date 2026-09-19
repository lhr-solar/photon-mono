#include "export.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

namespace photon::pexport {
namespace {

std::string pad4(std::uint32_t n) {
    std::string s = std::to_string(n);
    while (s.size() < 4) {
        s.insert(s.begin(), '0');
    }
    return s;
}

// Sequential page reader over one level's .pcol files. Index records are sorted
// by (signalId, startMs) and a signal's pages advance monotonically through the
// rotating column files, so caching the current handle is enough to make a whole
// level a near-sequential read.
class PageReader {
public:
    PageReader(std::filesystem::path dir, int level) : dir_(std::move(dir)), level_(level) {}

    bool read(const tsdb::IndexRecord& rec, std::vector<std::uint8_t>& out) {
        if (!in_.is_open() || fileNo_ != rec.fileNo) {
            in_.close();
            in_.clear();
            in_.open(dir_ / ("l" + std::to_string(level_)) / (pad4(rec.fileNo) + ".pcol"), std::ios::binary);
            if (!in_) {
                return false;
            }
            fileNo_ = rec.fileNo;
        }
        out.resize(rec.pageBytes);
        in_.seekg(static_cast<std::streamoff>(rec.offset), std::ios::beg);
        in_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(rec.pageBytes));
        if (!in_) {
            in_.clear();
            return false;
        }
        return true;
    }

private:
    std::filesystem::path dir_;
    int level_ = 0;
    std::ifstream in_;
    std::uint32_t fileNo_ = 0xFFFFFFFFu;
};

// 2^64, the weight of the high word of a 128-bit rollup sum.
constexpr double kTwo64 = 18446744073709551616.0;

}  // namespace

// The persisted sum is one or two 64-bit words (spec §5.1). Summing the words in
// double rather than converting a 128-bit integer keeps this free of the
// compiler-rt 128-bit-to-float builtins, which are not linked here.
//
// The unsigned case mirrors core's wideSumOf, which reads sumHi unconditionally.
// The signed case mirrors signedWideOf, including its reliance on the per-bucket
// wide flag, so a mean computed here agrees with one the query engine folds.
double meanRaw(const tsdb::RollupBucket& b, bool isSigned) {
    if (b.count == 0) {
        return 0.0;
    }
    double total = 0.0;
    if (isSigned && !b.wide) {
        total = static_cast<double>(b.sum);
    } else if (isSigned) {
        total = static_cast<double>(static_cast<std::int64_t>(b.sumHi)) * kTwo64 +
                static_cast<double>(static_cast<std::uint64_t>(b.sum));
    } else {
        total = static_cast<double>(b.sumHi) * kTwo64 + static_cast<double>(static_cast<std::uint64_t>(b.sum));
    }
    return total / static_cast<double>(b.count);
}

bool rawInRange(std::uint64_t tMs, std::uint64_t fromMs, std::uint64_t toMs) {
    return tMs >= fromMs && tMs <= toMs;
}

bool bucketInRange(std::uint64_t startMs, std::uint32_t bucketMs, std::uint64_t fromMs, std::uint64_t toMs) {
    if (bucketMs == 0) {
        return rawInRange(startMs, fromMs, toMs);
    }
    // A bucket ending exactly at fromMs is excluded; the first and last returned
    // buckets may extend past the window, which is deliberate (spec §6).
    return startMs <= toMs && startMs + bucketMs > fromMs;
}

int clampLevel(int requested, std::uint8_t builtLevels) {
    int want = std::clamp(requested, 0, tsdb::kLevelCount - 1);
    while (want > 0 && (builtLevels & static_cast<std::uint8_t>(1u << want)) == 0) {
        --want;
    }
    if ((builtLevels & static_cast<std::uint8_t>(1u << want)) == 0) {
        for (int L = 0; L < tsdb::kLevelCount; ++L) {
            if ((builtLevels & static_cast<std::uint8_t>(1u << L)) != 0) {
                return L;
            }
        }
    }
    return want;
}

int autoLevel(const tsdb::Store& store, std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
              std::uint32_t maxPoints) {
    return tsdb::pickLevel(fromMs, toMs, maxPoints, store.rawCount(signalId, fromMs, toMs),
                           store.levelsOf(signalId));
}

Report exportRange(const tsdb::Store& store, const Request& req, RowSink& sink) {
    Report rep;
    if (req.fromMs > req.toMs) {
        rep.error = "from_ms is after to_ms";
        return rep;
    }
    std::vector<std::uint8_t> page;
    std::vector<std::uint64_t> times;
    std::vector<std::int64_t> values;
    std::vector<tsdb::RollupBucket> slots;

    for (const auto id : req.signalIds) {
        const auto* info = store.signal(id);
        SignalReport sr;
        sr.signalId = id;
        const int level = req.level < 0 ? autoLevel(store, id, req.fromMs, req.toMs, req.maxPoints)
                                        : clampLevel(req.level, store.levelsOf(id));
        sr.level = level;
        sr.prior = store.prior(id, req.fromMs);
        sr.next = store.next(id, req.toMs);
        // The store rolls up in the raw integer domain. For Float/Double DBC
        // signals the raw value is a bit pattern, and ordering bit patterns
        // reverses negative floats, so min/max (and a sum over them) are not
        // recoverable. first/last are real samples and stay usable.
        const bool integerDomain = info == nullptr || info->valueType == dbc::ValueType::Integer;
        sr.envelopeSuppressed = level > 0 && !integerDomain;
        const bool isSigned = info == nullptr || info->isSigned;
        const auto bucketMs = tsdb::bucketMsOf(level);

        const auto& recs = store.index(level);
        PageReader reader(store.dir(), level);
        std::size_t i = tsdb::indexLowerBound(recs, id, req.fromMs);
        // A page that starts before the window can still hold in-range data.
        if (i > 0 && recs[i - 1].signalId == id && recs[i - 1].endMs >= req.fromMs) {
            --i;
        }
        for (; i < recs.size() && recs[i].signalId == id && recs[i].startMs <= req.toMs; ++i) {
            if (!reader.read(recs[i], page)) {
                rep.error = "cannot read page for signal " + std::to_string(id) + " at level " +
                            std::to_string(level);
                return rep;
            }
            ++sr.pages;
            sr.pageBytes += recs[i].pageBytes;
            rep.maxPageBytes = std::max<std::uint64_t>(rep.maxPageBytes, recs[i].pageBytes);
            tsdb::PageHeader hdr{};
            if (level == 0) {
                if (!tsdb::decodeRawPage(page, hdr, times, values)) {
                    rep.error = "corrupt L0 page for signal " + std::to_string(id);
                    return rep;
                }
                for (std::size_t k = 0; k < times.size() && k < values.size(); ++k) {
                    if (!rawInRange(times[k], req.fromMs, req.toMs)) {
                        continue;
                    }
                    ExportRow row;
                    row.signalId = id;
                    row.tMs = times[k];
                    row.level = 0;
                    row.bucketMs = 0;
                    row.hasValue = true;
                    row.raw = values[k];
                    row.value = info != nullptr ? tsdb::physical(*info, values[k])
                                                : static_cast<double>(values[k]);
                    if (!sink.row(row)) {
                        rep.error = "sink rejected a row";
                        return rep;
                    }
                    ++sr.rows;
                }
                continue;
            }
            if (!tsdb::decodeRollupPage(page, hdr, bucketMs, slots)) {
                rep.error = "corrupt L" + std::to_string(level) + " page for signal " + std::to_string(id);
                return rep;
            }
            for (const auto& slot : slots) {
                if (!slot.present || slot.count == 0) {
                    continue;
                }
                if (!bucketInRange(slot.startMs, bucketMs, req.fromMs, req.toMs)) {
                    continue;
                }
                ExportRow row;
                row.signalId = id;
                row.tMs = slot.startMs;
                row.level = level;
                row.bucketMs = bucketMs;
                row.hasEnvelope = true;
                row.count = slot.count;
                row.vFirst = info != nullptr ? tsdb::physical(*info, slot.first)
                                             : static_cast<double>(slot.first);
                row.vLast =
                    info != nullptr ? tsdb::physical(*info, slot.last) : static_cast<double>(slot.last);
                if (integerDomain) {
                    row.hasMinMax = true;
                    const double a = info != nullptr ? tsdb::physical(*info, slot.min)
                                                     : static_cast<double>(slot.min);
                    const double b = info != nullptr ? tsdb::physical(*info, slot.max)
                                                     : static_cast<double>(slot.max);
                    // The store's min/max are extremes of the raw integer, and a
                    // DBC factor may be negative (IMU_Accel_FR.Accel_X is
                    // -0.000122), which swaps them in physical units. Re-order
                    // after scaling or the envelope comes out inverted.
                    row.vMin = std::min(a, b);
                    row.vMax = std::max(a, b);
                    row.hasMean = true;
                    const double m = meanRaw(slot, isSigned);
                    row.vMean = info != nullptr ? m * info->factor + info->offset : m;
                }
                if (!sink.row(row)) {
                    rep.error = "sink rejected a row";
                    return rep;
                }
                ++sr.rows;
            }
        }
        rep.rows += sr.rows;
        rep.pages += sr.pages;
        rep.pageBytes += sr.pageBytes;
        rep.signals.push_back(std::move(sr));
    }
    rep.ok = true;
    return rep;
}

std::vector<std::uint32_t> signalsWithData(const tsdb::Store& store) {
    std::set<std::uint32_t> ids;
    for (int L = 0; L < tsdb::kLevelCount; ++L) {
        for (const auto& rec : store.index(L)) {
            ids.insert(rec.signalId);
        }
    }
    return {ids.begin(), ids.end()};
}

std::string signalKey(const tsdb::SignalInfo& info) {
    if (info.message.empty()) {
        return info.name;
    }
    return info.message + "." + info.name;
}

std::vector<std::uint32_t> resolveSignalSelectors(const tsdb::Store& store,
                                                  const std::vector<std::string>& selectors) {
    std::vector<std::uint32_t> ids;
    std::set<std::uint32_t> seen;
    for (const auto& sel : selectors) {
        if (sel.empty() || ids.size() >= 256) {
            continue;
        }
        const tsdb::SignalInfo* exact = nullptr;
        const tsdb::SignalInfo* nameOnly = nullptr;
        int nameHits = 0;
        for (const auto& [id, info] : store.signals()) {
            (void)id;
            if (signalKey(info) == sel) {
                exact = &info;
                break;
            }
            if (info.name == sel) {
                nameOnly = &info;
                ++nameHits;
            }
        }
        const tsdb::SignalInfo* hit = exact != nullptr ? exact : (nameHits == 1 ? nameOnly : nullptr);
        if (hit == nullptr || !seen.insert(hit->id).second) {
            continue;
        }
        ids.push_back(hit->id);
    }
    return ids;
}

void csvEscape(std::string* out, std::string_view field) {
    if (out == nullptr) {
        return;
    }
    bool quote = false;
    for (const char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            quote = true;
            break;
        }
    }
    if (!quote) {
        out->append(field);
        return;
    }
    out->push_back('"');
    for (const char c : field) {
        if (c == '"') {
            out->append("\"\"");
        } else {
            out->push_back(c);
        }
    }
    out->push_back('"');
}

bool writeCsv(const tsdb::Store& store, const Request& req,
              const std::function<bool(std::string_view)>& emit, Report* report) {
    if (!emit) {
        if (report != nullptr) {
            report->error = "csv emit is empty";
        }
        return false;
    }
    class Sink final : public RowSink {
    public:
        Sink(const tsdb::Store& st, const std::function<bool(std::string_view)>& em) : store(st), emit(em) {}

        bool row(const ExportRow& r) override {
            if (!header) {
                if (!emit("t_ms,message,signal,unit,value\n")) {
                    return false;
                }
                header = true;
            }
            double value = 0.0;
            if (r.hasValue) {
                value = r.value;
            } else if (r.hasMean) {
                value = r.vMean;
            } else if (r.hasEnvelope) {
                value = r.vLast;
            } else {
                return true;
            }
            const auto* info = store.signal(r.signalId);
            const std::string_view message = info != nullptr ? info->message : std::string_view{};
            const std::string_view name = info != nullptr ? info->name : std::string_view{};
            const std::string_view unit = info != nullptr ? info->unit : std::string_view{};
            line.clear();
            line.append(std::to_string(r.tMs));
            line.push_back(',');
            csvEscape(&line, message);
            line.push_back(',');
            csvEscape(&line, name);
            line.push_back(',');
            csvEscape(&line, unit);
            line.push_back(',');
            char buf[64];
            const int n = std::snprintf(buf, sizeof(buf), "%.9g", value);
            if (n > 0) {
                line.append(buf, static_cast<std::size_t>(n));
            }
            line.push_back('\n');
            return emit(line);
        }

        bool wroteHeader() const { return header; }

    private:
        const tsdb::Store& store;
        const std::function<bool(std::string_view)>& emit;
        std::string line;
        bool header = false;
    };

    Sink sink(store, emit);
    Report rep = exportRange(store, req, sink);
    if (rep.ok && !sink.wroteHeader()) {
        if (!emit("t_ms,message,signal,unit,value\n")) {
            rep.ok = false;
            rep.error = "csv header write failed";
        }
    }
    if (report != nullptr) {
        *report = rep;
    }
    return rep.ok;
}

bool validSessionId(std::string_view id) {
    if (id.empty() || id.size() > 80) {
        return false;
    }
    if (!std::isalnum(static_cast<unsigned char>(id.front()))) {
        return false;
    }
    for (const char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            continue;
        }
        return false;
    }
    return id.find("..") == std::string_view::npos;
}

std::string urlDecode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') {
                    return h - '0';
                }
                if (h >= 'a' && h <= 'f') {
                    return h - 'a' + 10;
                }
                if (h >= 'A' && h <= 'F') {
                    return h - 'A' + 10;
                }
                return -1;
            };
            const int a = hex(in[i + 1]);
            const int b = hex(in[i + 2]);
            if (a >= 0 && b >= 0) {
                out.push_back(static_cast<char>((a << 4) | b));
                i += 2;
                continue;
            }
        }
        if (c == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool parseCsvQuery(std::string_view target, CsvQuery* out) {
    if (out == nullptr) {
        return false;
    }
    *out = {};
    std::string_view q = target;
    const auto qpos = target.find('?');
    if (qpos != std::string_view::npos) {
        const auto path = target.substr(0, qpos);
        if (!path.empty() && path.find("export.csv") == std::string_view::npos) {
            out->error = "not an export path";
            return false;
        }
        q = target.substr(qpos + 1);
    }
    std::size_t start = 0;
    while (start <= q.size()) {
        const auto amp = q.find('&', start);
        const auto piece = q.substr(start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
        const auto eq = piece.find('=');
        if (eq != std::string_view::npos) {
            const std::string key = urlDecode(piece.substr(0, eq));
            const std::string val = urlDecode(piece.substr(eq + 1));
            if (key == "session") {
                out->session = val;
            } else if (key == "from") {
                out->fromMs = static_cast<std::uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
                out->haveFrom = true;
            } else if (key == "to") {
                out->toMs = static_cast<std::uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
                out->haveTo = true;
            } else if (key == "level") {
                out->level = std::atoi(val.c_str());
            } else if (key == "signals") {
                if (val == "all") {
                    out->allSignals = true;
                } else {
                    std::size_t p = 0;
                    while (p <= val.size() && out->selectors.size() < 256) {
                        const auto comma = val.find(',', p);
                        const auto part =
                            val.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
                        if (part == "all") {
                            out->allSignals = true;
                        } else if (!part.empty()) {
                            out->selectors.emplace_back(part);
                        }
                        if (comma == std::string::npos) {
                            break;
                        }
                        p = comma + 1;
                    }
                }
            } else if (key == "sig") {
                if (!val.empty() && out->selectors.size() < 256) {
                    out->selectors.push_back(val);
                }
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        start = amp + 1;
    }
    if (!validSessionId(out->session)) {
        out->error = out->session.empty() ? "missing session" : "bad session";
        return false;
    }
    if (out->level < 0 || out->level > 5) {
        out->level = 2;
    }
    return true;
}

}  // namespace photon::pexport
