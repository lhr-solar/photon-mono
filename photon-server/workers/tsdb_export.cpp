#include "export.hpp"
#include "base/json.hpp"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>

struct Sink : photon::pexport::RowSink {
    bool row(const photon::pexport::ExportRow& r) override {
        if (!r.hasValue) return false;
        // Strings preserve the entire integer bit pattern across JSON implementations.
        std::cout << "{\"type\":\"sample\",\"signal_id\":" << r.signalId << ",\"time_ms\":" << r.tMs
                  << ",\"raw\":\"" << r.raw << "\"}\n";
        return static_cast<bool>(std::cout);
    }
};
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: photon-tsdb-export SESSION_DIR\n"; return 2; }
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
