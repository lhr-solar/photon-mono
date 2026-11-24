#pragma once

#include "photon/control_v2.hpp"
#include "photon/protocol.hpp"
#include "photon/protocol_v2.hpp"
#include "photon_ui/catalog.hpp"

#include "photon/control.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Pure conversions from protocol v2 messages to the client's tables: no sockets,
// no UI. The client and the arena call them; the native tests call them too.
namespace photon::ui {

// Catalog fields the arena keeps beside ctrl::CatalogSignal (same order).
struct SignalExtra {
    int bus = -1;
    bool integer = false;
    int mux = -1;
    double periodMs = 0.0;
};

// A catalog as the client keeps it. Client id = server index + 1, so the
// client's "id 0 is unmapped" checks keep working. A second signal with the same
// "Message.Signal" name is left out (the first one is kept).
struct CatalogTable {
    std::vector<ctrl::CatalogSignal> signals;
    std::vector<SignalExtra> extra;  // parallel to signals
    std::vector<std::string> buses;
    std::uint32_t serverCount = 0;   // signals in the server's catalog, duplicates included
    std::uint32_t duplicates = 0;
    std::string firstDuplicate;      // the name of the first duplicate, for one log line
};

CatalogTable buildCatalogTable(const ctrl2::Catalog& catalog);

// Old client id to new client id, for every "Message.Signal" name in both tables.
std::unordered_map<std::uint32_t, std::uint32_t> remapIds(const std::vector<ctrl::CatalogSignal>& from,
                                                          const std::vector<ctrl::CatalogSignal>& to);

// Groups the records of a LiveSamples message by signal into telemetry blocks,
// one block per signal, in record order. Times are Unix ms: base_ns / 1e6 plus
// dt_us / 1e3. A record for an index >= serverCount, or a value that is not a
// finite number, is dropped.
std::vector<proto::TelemetryBlock> liveBlocks(const proto2::LiveSamples& live, std::uint32_t serverCount);

// The server indexes (client id - 1) to send in a subscribe, sorted. Ids that are
// 0 or past serverCount are left out: the server refuses a set with a stale index.
std::vector<std::uint32_t> subscribeIndexes(const std::unordered_set<std::uint32_t>& ids,
                                            std::uint32_t serverCount);

}  // namespace photon::ui
