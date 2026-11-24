#include "photon_ui/live_v2.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace photon::ui {

CatalogTable buildCatalogTable(const ctrl2::Catalog& catalog) {
    CatalogTable table;
    table.buses = catalog.buses;
    std::unordered_set<std::string> names;
    std::uint32_t index = 0;
    for (const auto& msg : catalog.messages) {
        for (const auto& sig : msg.signals) {
            // Server index i is client id i + 1: the subscribe message sends id - 1.
            const std::uint32_t id = index + 1;
            ++index;
            const std::string name = msg.name + "." + sig.name;
            if (!names.insert(name).second) {
                if (table.duplicates == 0) {
                    table.firstDuplicate = name;
                }
                ++table.duplicates;
                continue;
            }
            ctrl::CatalogSignal row;
            row.id = id;
            row.messageId = msg.id;
            row.message = msg.name;
            row.name = sig.name;
            row.unit = sig.unit;
            row.min = sig.min;
            row.max = sig.max;
            table.signals.push_back(std::move(row));
            SignalExtra extra;
            extra.bus = msg.bus;
            extra.integer = sig.integer;
            extra.mux = sig.mux;
            extra.periodMs = sig.periodMs;
            table.extra.push_back(extra);
        }
    }
    table.serverCount = index;
    return table;
}

std::unordered_map<std::uint32_t, std::uint32_t> remapIds(const std::vector<ctrl::CatalogSignal>& from,
                                                          const std::vector<ctrl::CatalogSignal>& to) {
    std::unordered_map<std::string, std::uint32_t> newIdByName;
    newIdByName.reserve(to.size());
    for (const auto& sig : to) {
        newIdByName.emplace(sig.message + "." + sig.name, sig.id);
    }
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    for (const auto& sig : from) {
        const auto it = newIdByName.find(sig.message + "." + sig.name);
        if (it != newIdByName.end()) {
            remap.emplace(sig.id, it->second);
        }
    }
    return remap;
}

std::vector<proto::TelemetryBlock> liveBlocks(const proto2::LiveSamples& live, std::uint32_t serverCount) {
    // Times in Unix ms as a double: 1.8e12 ms keeps about 0.25 us.
    const double baseMs = static_cast<double>(live.baseNs) / 1e6;
    std::vector<proto::TelemetryBlock> blocks;
    std::unordered_map<std::uint32_t, std::size_t> blockOf;
    for (const auto& rec : live.records) {
        if (rec.signal >= serverCount || !std::isfinite(rec.value)) {
            continue;
        }
        const std::uint32_t id = rec.signal + 1;
        auto [it, fresh] = blockOf.try_emplace(id, blocks.size());
        if (fresh) {
            blocks.emplace_back();
            blocks.back().signalId = id;
        }
        auto& block = blocks[it->second];
        block.timestamps.push_back(baseMs + static_cast<double>(rec.dtUs) / 1000.0);
        block.values.push_back(rec.value);
    }
    return blocks;
}

std::vector<std::uint32_t> subscribeIndexes(const std::unordered_set<std::uint32_t>& ids,
                                            std::uint32_t serverCount) {
    std::vector<std::uint32_t> indexes;
    indexes.reserve(ids.size());
    for (const std::uint32_t id : ids) {
        if (id >= 1 && id - 1 < serverCount) {
            indexes.push_back(id - 1);
        }
    }
    std::sort(indexes.begin(), indexes.end());
    return indexes;
}

}  // namespace photon::ui
