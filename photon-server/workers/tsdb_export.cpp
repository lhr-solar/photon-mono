#include "export.hpp"
#include "base/json.hpp"
#if __has_include("photon/protocol.hpp")
#include "photon/protocol.hpp"
#elif __has_include("wire/protocol.hpp")
#include "wire/protocol.hpp"
#else
#error "history chunk protocol header is missing"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

struct Sink : photon::pexport::RowSink {
    bool row(const photon::pexport::ExportRow& r) override {
        if (!r.hasValue) return false;
        // Strings preserve the entire integer bit pattern across JSON implementations.
        std::cout << "{\"type\":\"sample\",\"signal_id\":" << r.signalId << ",\"time_ms\":" << r.tMs
                  << ",\"raw\":\"" << r.raw << "\"}\n";
        return static_cast<bool>(std::cout);
    }
};
// CSV for the Logs dialog: t_ms,message,signal,unit,value on stdout.
// photon-tsdb-export --csv SESSION_DIR --level N [--all] [--sig Message.Signal ...]
int csvExport(int argc, char** argv) {
    std::string dir;
    int level = 2;
    bool all = false;
    std::vector<std::string> selectors;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << flag << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--level") {
            const char* value = need("--level");
            if (value == nullptr) {
                return 2;
            }
            level = std::atoi(value);
        } else if (arg == "--sig") {
            const char* value = need("--sig");
            if (value == nullptr) {
                return 2;
            }
            if (value[0] != '\0' && selectors.size() < 256) {
                selectors.emplace_back(value);
            }
        } else if (arg == "--all") {
            all = true;
        } else if (dir.empty() && !arg.empty() && arg[0] != '-') {
            dir = arg;
        } else {
            std::cerr << "unknown argument " << arg << "\n";
            return 2;
        }
    }
    if (dir.empty()) {
        std::cerr << "missing session dir\n";
        return 2;
    }
    if (level < 0 || level > 5) {
        level = 2;
    }

    photon::tsdb::Store store;
    std::string error;
    if (!store.open(dir, &error)) {
        std::cerr << (error.empty() ? "unknown session" : error) << "\n";
        return 2;
    }

    photon::pexport::Request req;
    req.fromMs = store.manifest().minMs;
    req.toMs = store.manifest().maxMs;
    req.level = level;
    req.maxPoints = 1u << 30;
    if (all || selectors.empty()) {
        req.signalIds = photon::pexport::signalsWithData(store);
    } else {
        req.signalIds = photon::pexport::resolveSignalSelectors(store, selectors);
        if (req.signalIds.empty()) {
            std::cerr << "no matching signals\n";
            return 2;
        }
    }

    std::size_t pending = 0;
    bool first = true;
    photon::pexport::Report report;
    const bool ok = photon::pexport::writeCsv(store, req, [&](std::string_view chunk) {
        std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        pending += chunk.size();
        if (first || pending >= 16 * 1024) {
            std::cout.flush();
            first = false;
            pending = 0;
        }
        return static_cast<bool>(std::cout);
    }, &report);
    std::cout.flush();
    if (!ok) {
        std::cerr << (report.error.empty() ? "export failed" : report.error) << "\n";
        return 2;
    }
    return 0;
}

namespace {

bool floatSignal(const photon::tsdb::SignalInfo* info) {
    return info != nullptr && (info->valueType == photon::dbc::ValueType::Float ||
                               info->valueType == photon::dbc::ValueType::Double);
}

double rollupMeanRaw(const photon::tsdb::RollupBucket& b, bool isSigned) {
    if (b.count == 0) {
        return 0.0;
    }
    if (!isSigned) {
        const double wide = std::ldexp(static_cast<double>(b.sumHi), 64) +
                            static_cast<double>(static_cast<std::uint64_t>(b.sum));
        return wide / static_cast<double>(b.count);
    }
    if (!b.wide) {
        return static_cast<double>(b.sum) / static_cast<double>(b.count);
    }
    const double wide = std::ldexp(static_cast<double>(static_cast<std::int64_t>(b.sumHi)), 64) +
                        static_cast<double>(static_cast<std::uint64_t>(b.sum));
    return wide / static_cast<double>(b.count);
}

void floatRollup(const photon::tsdb::SignalInfo& info, const photon::tsdb::RollupBucket& b, float& mn, float& mx,
                 float& mean) {
    bool have = false;
    auto take = [&](float v) {
        if (!std::isfinite(v)) {
            return;
        }
        if (!have) {
            mn = mx = v;
            have = true;
            return;
        }
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    };
    const float first = static_cast<float>(photon::tsdb::physical(info, b.first));
    const float last = static_cast<float>(photon::tsdb::physical(info, b.last));
    take(first);
    take(last);
    take(static_cast<float>(photon::tsdb::physical(info, b.min)));
    take(static_cast<float>(photon::tsdb::physical(info, b.max)));
    if (!have) {
        mn = mx = mean = 0.0f;
        return;
    }
    mean = std::isfinite(last) ? last : first;
}

struct ServedSeries {
    photon::json::Object meta;
    std::vector<std::vector<std::uint8_t>> chunks;
};

ServedSeries serveSeries(const photon::tsdb::Store& store, const photon::tsdb::SeriesResult& s, std::uint32_t queryId,
                         std::uint32_t& chunkSeq, bool finalSeries) {
    ServedSeries out;
    const photon::tsdb::SignalInfo* info = store.signal(s.signalId);
    auto phys = [&](std::int64_t raw) -> float {
        return info != nullptr ? static_cast<float>(photon::tsdb::physical(*info, raw)) : static_cast<float>(raw);
    };
    const bool raw = s.level == 0;
    const std::size_t n = raw ? s.timestamps.size() : s.buckets.size();
    photon::json::Object meta;
    meta.emplace("signal_id", s.signalId);
    meta.emplace("level", s.level);
    meta.emplace("bucket_ms", s.bucketMs);
    meta.emplace("points", static_cast<std::uint64_t>(n));
    std::uint64_t tMin = 0;
    std::uint64_t tMax = 0;
    if (raw && !s.timestamps.empty()) {
        tMin = s.timestamps.front();
        tMax = s.timestamps.back();
    } else {
        for (const auto& b : s.buckets) {
            if (!b.present || b.count == 0) {
                continue;
            }
            if (tMin == 0 || b.startMs < tMin) {
                tMin = b.startMs;
            }
            if (b.startMs > tMax) {
                tMax = b.startMs;
            }
        }
    }
    meta.emplace("t_min", tMin);
    meta.emplace("t_max", tMax);
    if (s.prior) {
        photon::json::Object p;
        p.emplace("t", s.prior->timestampMs);
        p.emplace("v", phys(s.prior->raw));
        meta.emplace("prior", photon::json::Value(std::move(p)));
    }
    if (s.next) {
        photon::json::Object nx;
        nx.emplace("t", s.next->timestampMs);
        nx.emplace("v", phys(s.next->raw));
        meta.emplace("next", photon::json::Value(std::move(nx)));
    }
    constexpr std::size_t kMaxChunkSlots = 2000;
    std::size_t off = 0;
    if (n == 0) {
        meta.emplace("chunks", 0);
        out.meta = std::move(meta);
        return out;
    }
    while (off < n) {
        const std::size_t take = std::min(kMaxChunkSlots, n - off);
        photon::proto::HistoryChunk chunk;
        chunk.queryId = queryId;
        chunk.signalId = s.signalId;
        chunk.level = static_cast<std::uint8_t>(s.level);
        chunk.chunkSeq = chunkSeq++;
        const bool last = finalSeries && off + take >= n;
        if (raw) {
            chunk.fields = photon::proto::kHistRaw;
            chunk.flags = photon::proto::kHistDense;
            chunk.bucketMs = 0;
            chunk.baseMs = s.timestamps[off];
            chunk.slotCount = static_cast<std::uint32_t>(take);
            chunk.valueCount = static_cast<std::uint32_t>(take);
            chunk.timestamps.reserve(take);
            chunk.rawV.reserve(take);
            for (std::size_t i = 0; i < take; ++i) {
                chunk.timestamps.push_back(static_cast<double>(s.timestamps[off + i]));
                chunk.rawV.push_back(phys(s.raw[off + i]));
            }
        } else {
            chunk.fields = static_cast<std::uint8_t>(photon::proto::kHistMin | photon::proto::kHistMax | photon::proto::kHistMean);
            chunk.bucketMs = s.bucketMs;
            chunk.baseMs = s.buckets[off].startMs;
            chunk.slotCount = static_cast<std::uint32_t>(take);
            bool dense = true;
            for (std::size_t i = 0; i < take; ++i) {
                const auto& b = s.buckets[off + i];
                if (!b.present || b.count == 0) {
                    dense = false;
                    break;
                }
            }
            if (dense) {
                chunk.flags = photon::proto::kHistDense;
            } else {
                chunk.presence.assign((take + 7) / 8, 0);
                for (std::size_t i = 0; i < take; ++i) {
                    const auto& b = s.buckets[off + i];
                    if (b.present && b.count > 0) {
                        chunk.presence[i / 8] = static_cast<std::uint8_t>(chunk.presence[i / 8] | (1u << (i % 8)));
                    }
                }
            }
            for (std::size_t i = 0; i < take; ++i) {
                const auto& b = s.buckets[off + i];
                if (!b.present || b.count == 0) {
                    continue;
                }
                float mn = 0.0f;
                float mx = 0.0f;
                float mean = 0.0f;
                if (floatSignal(info)) {
                    floatRollup(*info, b, mn, mx, mean);
                } else {
                    mn = phys(b.min);
                    mx = phys(b.max);
                    if (info != nullptr && info->factor < 0.0) {
                        std::swap(mn, mx);
                    }
                    const double meanRaw = rollupMeanRaw(b, info == nullptr || info->isSigned);
                    mean = info != nullptr ? static_cast<float>(meanRaw * info->factor + info->offset) : static_cast<float>(meanRaw);
                }
                chunk.minV.push_back(mn);
                chunk.maxV.push_back(mx);
                chunk.meanV.push_back(mean);
            }
            chunk.valueCount = static_cast<std::uint32_t>(chunk.minV.size());
        }
        if (last) {
            chunk.flags = static_cast<std::uint16_t>(chunk.flags | photon::proto::kHistFinal);
        }
        off += take;
        out.chunks.push_back(photon::proto::encodeHistoryChunk(chunk));
    }
    meta.emplace("chunks", static_cast<int>(out.chunks.size()));
    out.meta = std::move(meta);
    return out;
}

void writeError(const std::string& message) {
    photon::json::Object obj;
    obj.emplace("ok", false);
    obj.emplace("error", message);
    std::cout << photon::json::stringify(photon::json::Value(std::move(obj))) << '\n';
    std::cout.flush();
}

int historyServe(const std::string& dir) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    photon::tsdb::Store store;
    std::string error;
    if (!store.open(dir, &error)) {
        std::cerr << (error.empty() ? "unknown session" : error) << '\n';
        return 2;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        if (line == "quit") {
            break;
        }
        photon::json::ParseError perr;
        const auto parsed = photon::json::parse(line, &perr);
        if (!parsed || !parsed->isObject()) {
            writeError(perr.message.empty() ? "bad history request" : perr.message);
            continue;
        }
        const auto& req = *parsed;
        photon::tsdb::Query q;
        q.fromMs = static_cast<std::uint64_t>(req["from_ms"].asInt());
        q.toMs = static_cast<std::uint64_t>(req["to_ms"].asInt());
        q.maxPoints = static_cast<std::uint32_t>(req["max_points"].asInt());
        if (q.maxPoints == 0 || q.maxPoints > 8000) {
            q.maxPoints = 8000;
        }
        const auto queryId = static_cast<std::uint32_t>(req["query_id"].asInt());
        if (req["signal_ids"].isArray()) {
            for (const auto& id : req["signal_ids"].asArray()) {
                if (q.signalIds.size() >= 512) {
                    break;
                }
                const auto n = static_cast<std::uint32_t>(id.asInt());
                if (n != 0) {
                    q.signalIds.push_back(n);
                }
            }
        }
        if (q.signalIds.empty() || q.fromMs > q.toMs) {
            writeError("signal_ids required");
            continue;
        }
        const auto result = store.query(q);
        photon::json::Array series;
        std::vector<std::vector<std::uint8_t>> chunks;
        std::uint32_t seq = 0;
        std::size_t nonempty = 0;
        for (const auto& s : result.series) {
            const bool raw = s.level == 0;
            const std::size_t n = raw ? s.timestamps.size() : s.buckets.size();
            if (n != 0) {
                ++nonempty;
            }
        }
        std::size_t seen = 0;
        int level = result.level;
        std::uint32_t bucket = result.bucketMs;
        for (const auto& s : result.series) {
            const bool raw = s.level == 0;
            const std::size_t n = raw ? s.timestamps.size() : s.buckets.size();
            const bool finalSeries = n != 0 && ++seen == nonempty;
            auto served = serveSeries(store, s, queryId, seq, finalSeries);
            series.emplace_back(photon::json::Value(std::move(served.meta)));
            chunks.insert(chunks.end(), served.chunks.begin(), served.chunks.end());
        }
        photon::json::Object header;
        header.emplace("ok", true);
        header.emplace("level", level);
        header.emplace("bucket_ms", bucket);
        header.emplace("truncated", result.truncated);
        header.emplace("chunks", static_cast<int>(chunks.size()));
        header.emplace("series", photon::json::Value(std::move(series)));
        std::cout << photon::json::stringify(photon::json::Value(std::move(header))) << '\n';
        for (const auto& chunk : chunks) {
            const auto n = static_cast<std::uint32_t>(chunk.size());
            const unsigned char len[4] = {
                static_cast<unsigned char>(n & 0xff),
                static_cast<unsigned char>((n >> 8) & 0xff),
                static_cast<unsigned char>((n >> 16) & 0xff),
                static_cast<unsigned char>((n >> 24) & 0xff),
            };
            std::cout.write(reinterpret_cast<const char*>(len), 4);
            std::cout.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        }
        std::cout.flush();
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--csv") {
        return csvExport(argc, argv);
    }
    if (argc >= 3 && std::string(argv[1]) == "--serve") {
        return historyServe(argv[2]);
    }
    if (argc != 2) {
        std::cerr << "usage: photon-tsdb-export SESSION_DIR\n"
                     "       photon-tsdb-export --csv SESSION_DIR --level N [--all] [--sig Message.Signal ...]\n"
                     "       photon-tsdb-export --serve SESSION_DIR\n";
        return 2;
    }
    photon::tsdb::Store store; std::string error;
    if (!store.open(argv[1], &error)) { std::cerr << error; return 2; }
    photon::json::Object header; header.emplace("type", "header");
    header.emplace("session_id", store.manifest().id);
    header.emplace("dbc_hash", store.manifest().dbcSha);
    header.emplace("min_ms", store.manifest().minMs); header.emplace("max_ms", store.manifest().maxMs);
    photon::json::Array defs;
    for (const auto& [id, d] : store.signals()) {
        photon::json::Object obj; obj.emplace("id", id); obj.emplace("message", d.message);
        obj.emplace("name", d.name); obj.emplace("unit", d.unit); obj.emplace("factor", d.factor);
        obj.emplace("offset", d.offset); obj.emplace("bits", d.bits); obj.emplace("signed", d.isSigned);
        obj.emplace("value_type", static_cast<int>(d.valueType)); defs.emplace_back(std::move(obj));
    }
    header.emplace("definitions", std::move(defs));
    std::cout << photon::json::stringify(photon::json::Value(std::move(header))) << '\n';
    photon::pexport::Request req; req.fromMs = store.manifest().minMs; req.toMs = store.manifest().maxMs;
    const auto ids = photon::pexport::signalsWithData(store);
    for (auto id : ids) {
        const auto levels = store.levelsOf(id);
        if (levels & 1) {
            req.level = 0; req.signalIds = {id}; Sink sink;
            auto report = photon::pexport::exportRange(store, req, sink);
            if (!report.ok) { std::cerr << report.error; return 2; }
            continue;
        }
        int level = 1; while (level <= 5 && !(levels & (1u << level))) ++level;
        if (level > 5) continue;
        for (const auto& rec : store.index(level)) {
            if (rec.signalId != id) continue;
            std::ostringstream file; file << std::setw(4) << std::setfill('0') << rec.fileNo << ".pcol";
            std::ifstream pageFile(store.dir() / ("l" + std::to_string(level)) / file.str(), std::ios::binary);
            if (!pageFile || rec.pageBytes > 4 * 1024 * 1024) return 2;
            std::vector<std::uint8_t> page(rec.pageBytes);
            pageFile.seekg(rec.offset); pageFile.read(reinterpret_cast<char*>(page.data()), rec.pageBytes);
            photon::tsdb::PageHeader ph; std::uint32_t bucketMs = 0;
            std::vector<photon::tsdb::RollupBucket> buckets;
            if (!pageFile || !photon::tsdb::decodeRollupPage(page, ph, bucketMs, buckets)) return 2;
            for (const auto& b : buckets) if (b.present && b.count) {
                std::cout << "{\"type\":\"bucket\",\"signal_id\":" << id << ",\"time_ms\":" << b.startMs
                          << ",\"bucket_ms\":" << bucketMs << ",\"count\":" << b.count
                          << ",\"min_raw\":\"" << b.min << "\",\"max_raw\":\"" << b.max
                          << "\",\"sum_lo\":\"" << b.sum << "\",\"sum_hi\":\"" << b.sumHi
                          << "\",\"first_raw\":\"" << b.first << "\",\"last_raw\":\"" << b.last
                          << "\",\"wide\":" << (b.wide ? "true" : "false") << "}\n";
            }
        }
    }
}
