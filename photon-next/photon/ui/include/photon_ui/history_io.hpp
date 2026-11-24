#pragma once

#include <cstdint>
#include <string>

namespace photon::ui {

// Where a TileCache sends its tile requests. The client implements it on the v2
// socket (through TileRequestQueue); analysis uses a recorder that forwards to it.
class HistoryIO {
public:
    virtual ~HistoryIO() = default;
    // One tile of a run. signalId is the client id (catalog index + 1). urgent is
    // true for tiles under the visible plot or the cursor; the rest are priming.
    virtual void requestTile(std::uint32_t queryId, const std::string& runId, std::uint32_t signalId, int level,
                             std::int64_t index, bool urgent) = 0;
    virtual void cancelTile(std::uint32_t queryId) = 0;
    // True when the transport will not take more requests now. The cache then
    // cancels tiles that left the view instead of queueing more.
    virtual bool queueFull() const { return false; }
};

}  // namespace photon::ui
