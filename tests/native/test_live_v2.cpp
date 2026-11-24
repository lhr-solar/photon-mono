#include "photon/control_v2.hpp"
#include "photon/protocol_v2.hpp"
#include "photon_ui/catalog.hpp"
#include "photon_ui/live_v2.hpp"
#include "photon_ui/mux.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

namespace {

photon::ctrl2::Catalog sampleCatalog() {
    using namespace photon::ctrl2;
    Catalog cat;
    cat.sha = "abc123";
    cat.buses = {"CarCAN", "DAqCAN"};
    CatalogMessage bps;
    bps.id = 0x100;
    bps.name = "BPSPackData";
    bps.bus = 0;
    bps.signals.push_back(CatalogSignal{0, "CellSumVoltage", "V", 0.0, 655.35, false, -1, 100.0});
    bps.signals.push_back(CatalogSignal{1, "Pack_Current", "A", -500.0, 500.0, false, -1, 0.0});
    cat.messages.push_back(bps);
    CatalogMessage mux;
    mux.id = 0x200;
    mux.name = "BPS_Voltage_Aggregate_Arr";
    mux.bus = 1;
    mux.signals.push_back(CatalogSignal{2, "BPS_Tap_idx", "", 0.0, 31.0, true, -1, 0.0});
    mux.signals.push_back(CatalogSignal{3, "BPS_Tap_V", "V", 0.0, 5.0, false, 3, 10.0});
    cat.messages.push_back(mux);
    return cat;
}

}  // namespace

void runLiveV2Tests() {
    using namespace photon;
    using namespace photon::ui;

    {
        // Index 0 becomes client id 1, and ids follow the server's index order.
        const auto table = buildCatalogTable(sampleCatalog());
        CHECK(table.signals.size() == 4);
        CHECK(table.serverCount == 4);
        CHECK(table.duplicates == 0);
        CHECK(table.signals.size() == table.extra.size());
        CHECK(table.signals[0].id == 1);
        CHECK(table.signals[0].message == "BPSPackData");
        CHECK(table.signals[0].messageId == 0x100);
        CHECK(table.signals[0].name == "CellSumVoltage");
        CHECK(table.signals[0].unit == "V");
        CHECK(near(table.signals[0].max, 655.35));
        CHECK(table.signals[3].id == 4);
        CHECK(table.signals[3].messageId == 0x200);
        CHECK(table.extra[0].bus == 0);
        CHECK(table.extra[0].periodMs == 100.0);
        CHECK(table.extra[0].mux == -1);
        CHECK(table.extra[3].bus == 1);
        CHECK(table.extra[3].mux == 3);
        CHECK(table.extra[2].integer);
        CHECK(!table.extra[0].integer);
        CHECK(table.buses.size() == 2);
    }

    {
        // Client ids never reach the synthetic fallback range.
        const auto table = buildCatalogTable(sampleCatalog());
        for (const auto& sig : table.signals) {
            CHECK(sig.id != kSignalRpm);
            CHECK(sig.id != kSignalSpeed);
            CHECK(sig.id != kSignalSoc);
            CHECK(sig.id != kSignalTemp);
            CHECK(sig.id != 0);
        }
        CHECK(kSignalRpm == 0xFFFFFF01u);
        CHECK(kSignalTemp == 0xFFFFFF04u);
    }

    {
        // A repeated "Message.Signal" keeps the first; the index still counts, so the
        // later signals keep their server indexes.
        auto cat = sampleCatalog();
        cat.messages[1].name = "BPSPackData";
        cat.messages[1].signals[0].name = "CellSumVoltage";
        const auto table = buildCatalogTable(cat);
        CHECK(table.duplicates == 1);
        CHECK(table.firstDuplicate == "BPSPackData.CellSumVoltage");
        CHECK(table.serverCount == 4);
        CHECK(table.signals.size() == 3);
        CHECK(table.signals[0].id == 1);
        CHECK(table.signals[1].id == 2);
        CHECK(table.signals[2].id == 4);  // server index 3 keeps id 4
    }

    {
        // Decoding off: an empty catalog has no signals and no ids to subscribe.
        ctrl2::Catalog empty;
        const auto table = buildCatalogTable(empty);
        CHECK(table.signals.empty());
        CHECK(table.serverCount == 0);
        CHECK(subscribeIndexes({1, 2, 3}, table.serverCount).empty());
    }

    {
        // A new catalog moves a wanted signal to its new id by name; a signal that
        // is gone is not in the map.
        const auto oldTable = buildCatalogTable(sampleCatalog());
        auto next = sampleCatalog();
        next.sha = "def456";
        // Insert a signal at the front: every index moves by one.
        next.messages[0].signals.insert(next.messages[0].signals.begin(),
                                        ctrl2::CatalogSignal{0, "Added", "", 0.0, 1.0, false, -1, 0.0});
        for (std::uint32_t i = 0; i < next.messages[0].signals.size(); ++i) {
            next.messages[0].signals[i].index = i;
        }
        // Drop BPS_Tap_V: its name is gone.
        next.messages[1].signals.pop_back();
        const auto newTable = buildCatalogTable(next);
        const auto remap = remapIds(oldTable.signals, newTable.signals);
        CHECK(remap.count(1) == 1 && remap.at(1) == 2);   // CellSumVoltage: id 1 -> 2
        CHECK(remap.count(2) == 1 && remap.at(2) == 3);   // Pack_Current: id 2 -> 3
        CHECK(remap.count(3) == 1 && remap.at(3) == 4);   // BPS_Tap_idx: id 3 -> 4 (its index is now 3)
        CHECK(remap.count(4) == 0);                       // BPS_Tap_V is gone
    }

    {
        // Records are grouped by signal in record order; times are base + dt.
        proto2::LiveSamples live;
        live.baseNs = 1'800'000'000'000'000'000;  // Unix ns
        live.records.push_back({0, 0, 1.5});      // signal 0 -> id 1
        live.records.push_back({0, 1, -2.0});     // signal 1 -> id 2
        live.records.push_back({2500, 0, 2.5});   // 2.5 ms later
        live.records.push_back({2500, 1, -3.0});
        const auto blocks = liveBlocks(live, 4);
        CHECK(blocks.size() == 2);
        CHECK(blocks[0].signalId == 1);
        CHECK(blocks[1].signalId == 2);
        CHECK(blocks[0].timestamps.size() == 2);
        CHECK(blocks[0].values.size() == 2);
        const double base = 1'800'000'000'000'000'000.0 / 1e6;
        CHECK(near(blocks[0].timestamps[0], base, 1e-3, 1e-12));
        CHECK(near(blocks[0].timestamps[1], base + 2.5, 1e-3, 1e-12));
        CHECK(near(blocks[0].values[1], 2.5));
        CHECK(near(blocks[1].values[0], -2.0));
    }

    {
        // Records for an index past the catalog, and NaN or infinite values, are dropped.
        proto2::LiveSamples live;
        live.baseNs = 1'000'000'000;
        live.records.push_back({0, 0, std::numeric_limits<double>::quiet_NaN()});
        live.records.push_back({0, 0, std::numeric_limits<double>::infinity()});
        live.records.push_back({0, 9, 7.0});  // index 9 is not in a 4-signal catalog
        live.records.push_back({0, 3, 4.0});  // index 3 -> id 4
        const auto blocks = liveBlocks(live, 4);
        CHECK(blocks.size() == 1);
        CHECK(blocks[0].signalId == 4);
        CHECK(blocks[0].values.size() == 1);
        CHECK(near(blocks[0].values[0], 4.0));
        CHECK(liveBlocks(proto2::LiveSamples{}, 4).empty());
    }

    {
        // Subscribe: client ids in, sorted server indexes out. Id 0 and ids past the
        // catalog are left out.
        const std::unordered_set<std::uint32_t> ids{4, 0, 2, 9, 1};
        const auto indexes = subscribeIndexes(ids, 4);
        CHECK(indexes.size() == 3);
        CHECK(indexes[0] == 0);
        CHECK(indexes[1] == 1);
        CHECK(indexes[2] == 3);
        CHECK(subscribeIndexes({}, 4).empty());
    }

    {
        // Records of one CAN frame share a time. Through the wire and the blocks,
        // the mux cell pairing still fills all 32 taps.
        proto2::LiveSamples live;
        live.baseNs = 5'000'000'000'000;  // 5000 s, a whole number of ms
        for (std::uint32_t tap = 0; tap < 32; ++tap) {
            live.records.push_back({0, 2, static_cast<double>(tap)});                // tap index
            live.records.push_back({0, 3, 3.200 + 0.010 * static_cast<double>(tap)});  // tap voltage
        }
        const auto frame = proto2::encodeLiveSamples(live, 7);
        const auto decoded = proto2::decodeLiveSamples(frame);
        CHECK(decoded.has_value());
        if (!decoded) {
            return;
        }
        const auto blocks = liveBlocks(*decoded, 4);
        CHECK(blocks.size() == 2);
        const auto& idx = blocks[0];   // id 3 (server index 2)
        const auto& val = blocks[1];   // id 4 (server index 3)
        CHECK(idx.signalId == 3);
        CHECK(val.signalId == 4);
        CHECK(idx.values.size() == 32);
        std::vector<float> out;
        applyMuxSeries(idx.timestamps.data(), idx.values.data(), idx.values.size(), val.timestamps.data(),
                       val.values.data(), val.values.size(), out, nullptr, 32, true);
        CHECK(out.size() == 32);
        int filled = 0;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (out[i] > 0.05f) {
                ++filled;
                CHECK(near(static_cast<double>(out[i]), 3.200 + 0.010 * static_cast<double>(i), 1e-4));
            }
        }
        CHECK(filled == 32);
    }
}
