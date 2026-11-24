#pragma once

#include "photon/protocol_v2.hpp"
#include "photon_ui/history_io.hpp"
#include "photon_ui/tile_levels.hpp"
#include "photon_ui/tile_store.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace photon::ui {

// Protocol v2 tiles of one run. The server decodes; a tile arrives as pieces (Tile
// 0x22) that the cache assembles by offset until the last piece.
//   Requested  a tile_request is out; pieces are being assembled
//   Open       complete, not sealed yet (the live edge): asked again every kOpenRefreshMs
//   Sealed     complete and sealed: never changes for this run's rev
//   Empty      complete with no samples (sealed or not)
//   Unknown    the last request failed: asked again after a backoff (1 s .. 30 s)
// A tile in the map with haveData set has samples the plot can use, whatever its
// state: a refresh that is still in flight does not hide the older data.
enum class TileState : std::uint8_t { Requested, Open, Sealed, Empty, Unknown };

struct TileKey {
    std::string runId;
    std::int64_t rev = 0;
    std::string sha;
    std::uint32_t signalId = 0;  // client id (catalog index + 1)
    std::uint8_t level = 0;
    std::int64_t tileIndex = 0;

    bool operator==(const TileKey& o) const {
        return signalId == o.signalId && level == o.level && tileIndex == o.tileIndex && rev == o.rev &&
               runId == o.runId && sha == o.sha;
    }
};

struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const {
        std::size_t h = k.signalId;
        h ^= static_cast<std::size_t>(k.level) << 24;
        h ^= static_cast<std::size_t>(k.tileIndex) * 2654435761u;
        h ^= static_cast<std::size_t>(k.rev) * 40503u;
        h ^= std::hash<std::string>{}(k.runId);
        h ^= std::hash<std::string>{}(k.sha) << 1;
        return h;
    }
};

struct Tile {
    TileKey key;
    TileState state = TileState::Unknown;
    bool haveData = false;  // the arrays below hold a complete response
    bool sealed = false;
    std::uint32_t qid = 0;  // the request in flight, when Requested
    double lastRequestMs = 0.0;
    double retryAtMs = 0.0;
    std::uint32_t failures = 0;
    mutable std::uint64_t lastUsed = 0;
    std::size_t bytes = 0;
    std::vector<double> t;     // Unix ms of each sample
    std::vector<float> rawV;   // level 0: the value
    std::vector<float> minV;   // level > 0
    std::vector<float> maxV;
    std::vector<float> meanV;
};

struct PlotArrays {
    std::vector<double> t;
    std::vector<double> y;
    std::vector<double> yMin;
    std::vector<double> yMax;
    bool envelope = false;
    // True when part of the window had no tile yet. Those spans are drawn from a
    // coarser tile where one is cached, and otherwise left empty (loading).
    bool incomplete = false;
};

class TileCache {
public:
    static constexpr std::size_t kDefaultBudget = 256 * 1024 * 1024;
    // Live-edge tiles are asked again this often while they are visible.
    static constexpr double kOpenRefreshMs = 2000.0;
    static constexpr double kRetryFirstMs = 1000.0;
    static constexpr double kRetryMaxMs = 30000.0;
    // A window wider than this many tiles at its level is cut to its newest end.
    static constexpr std::int64_t kMaxTilesPerWindow = 64;
    // A disk read that has not answered in this many persistence pumps is abandoned
    // for the network.
    static constexpr std::uint64_t kDiskDeadlinePumps = 240;
    static constexpr std::size_t kWriteQueueCap = 256;
    static constexpr std::size_t kWritesPerPump = 24;

    // Milliseconds on a monotonic clock. Tests replace it.
    using Clock = double (*)();
    static void setClock(Clock clock);

    // Overrides the process-wide default store. nullptr disables persistence.
    void setTileStore(TileStore* store);
    // Bucket widths from hello_ack.levels. Nothing is requested until these are set.
    void setLevels(std::vector<std::int64_t> bucketMs);
    const TileLevels& levels() const { return levels_; }
    // Client id to catalog "period_ms" (0 unknown), for drawing gaps.
    void setPeriods(std::unordered_map<std::uint32_t, double> periodMs) { periodMs_ = std::move(periodMs); }

    // The run whose tiles this cache holds. A change of run, rev or sha drops every
    // tile and every pending request; a change of min or max only moves the span.
    void setRun(const std::string& runId, std::int64_t rev, const std::string& sha, std::uint64_t minMs,
                std::uint64_t maxMs);
    const std::string& runId() const { return runId_; }
    std::int64_t rev() const { return rev_; }
    const std::string& catalogSha() const { return sha_; }
    std::uint64_t runMinMs() const { return minMs_; }
    std::uint64_t runMaxMs() const { return maxMs_; }

    // The live ring's oldest sample per signal. Tiles from that time on are not used:
    // the ring draws them.
    void updateRingOldest(std::uint32_t signalId, std::uint64_t oldestMs);
    void clearRingOldest() { ringOldest_.clear(); }

    // Requests the tiles covering [fromMs, toMs] at the level that fits pixels
    // (chooseLevel). Nothing is sent while dragging.
    void requestWindow(HistoryIO& io, std::span<const std::uint32_t> signalIds, std::uint64_t fromMs,
                       std::uint64_t toMs, std::uint32_t pixels, bool dragging);
    // The raw tile at cursorMs (and the one before it when the cursor is near its start).
    void requestCursorWindow(HistoryIO& io, std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs);
    // Priming at startup: the cursor's raw tiles, not urgent.
    void primeStartup(HistoryIO& io, std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs);
    // Cancels in-flight tiles that do not overlap [fromMs, toMs]. Only when the
    // transport's queue is full, so a slow link does not cancel what it could still serve.
    void cancelLeaving(HistoryIO& io, std::uint64_t fromMs, std::uint64_t toMs);
    // Cancels every request in flight, then forgets them (run change, close).
    void cancelAll(HistoryIO& io);
    // Forgets every request in flight without telling anyone (the socket is gone).
    void abandonRequests();
    // The live stream skipped [fromMs, toMs]: unsealed tiles there are asked again now.
    void noteGap(std::uint64_t fromMs, std::uint64_t toMs);
    bool owns(std::uint32_t qid) const { return inflight_.contains(qid); }

    // Piece and error routing. Both return false when the qid is not in flight here
    // (cancelled, failed, or another cache's), so the caller drops the message.
    bool ingestPiece(const proto2::TilePiece& piece);
    bool ingestError(std::uint32_t qid);

    bool sampleAt(std::uint32_t signalId, std::uint64_t tMs, double& value) const;
    // Nearest raw sample within slackMs of tMs.
    bool sampleNear(std::uint32_t signalId, std::uint64_t tMs, double& value,
                    std::uint64_t slackMs = 120000) const;
    // First |value| >= minAbs in any resident raw tile. Used when the playhead sits
    // on 0,0 before a GPS fix.
    bool sampleFirstAbs(std::uint32_t signalId, double& value, double minAbs) const;
    // Plot arrays for a window at the level that fits maxPoints pixels, with line
    // breaks at gaps (see the gap rules in protocol-v2.md).
    PlotArrays gatherPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                          std::uint32_t maxPoints = 0) const;
    // Raw values only, no breaks. Used by threshold crossings and cell pairing.
    PlotArrays gatherL0(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const;
    // Appends the live ring after the tile samples in dest.
    void stitchRing(PlotArrays& dest, std::span<const double> t, std::span<const double> v, std::size_t count,
                    std::size_t cap, std::size_t write, std::uint64_t fromMs, std::uint64_t toMs,
                    std::uint32_t signalId) const;

    // Analysis-owned caches start query ids in a reserved high range so they never
    // collide with the arena's (0 keeps the live default, 1).
    void setQueryOrigin(std::uint32_t origin) { nextQuery_ = origin == 0 ? 1u : origin; }
    std::uint32_t nextQueryId() const { return nextQuery_; }

    std::uint64_t pump();  // persistence: restores, writes and disk deadlines
    std::size_t residentBytes() const { return usedBytes_; }
    std::size_t overshootBytes() const { return usedBytes_ > budget_ ? usedBytes_ - budget_ : 0; }
    std::size_t usedBytes() const { return usedBytes_; }
    std::size_t budget() const { return budget_; }
    void setBudget(std::size_t bytes) { budget_ = bytes == 0 ? kDefaultBudget : bytes; }
    std::size_t tileCount() const { return tiles_.size(); }
    std::size_t inflightCount() const { return inflight_.size(); }
    const Tile* find(std::uint32_t signalId, int level, std::int64_t index) const;

    std::size_t persistRestoredCount() const { return persistRestored_; }
    std::size_t persistWrittenCount() const { return persistWritten_; }
    std::size_t persistPendingReads() const { return disk_.size(); }

private:
    struct InFlight {
        TileKey key;
        std::uint32_t filled = 0;  // samples assembled so far (the next offset)
        bool sealed = false;
        double startedMs = 0.0;
        std::vector<double> t;
        std::vector<float> rawV;
        std::vector<float> minV;
        std::vector<float> maxV;
        std::vector<float> meanV;
    };

    struct DiskRead {
        std::uint64_t deadlinePump = 0;
        bool pending = false;
    };

    TileLevels levels_;
    std::string runId_;
    std::int64_t rev_ = 0;
    std::string sha_;
    std::uint64_t minMs_ = 0;
    std::uint64_t maxMs_ = 0;
    std::unordered_map<std::uint32_t, double> periodMs_;
    std::unordered_map<std::uint32_t, std::uint64_t> ringOldest_;
    std::unordered_map<TileKey, Tile, TileKeyHash> tiles_;
    std::unordered_map<std::uint32_t, InFlight> inflight_;
    std::unordered_map<TileKey, DiskRead, TileKeyHash> disk_;
    std::vector<TileKey> writeQueue_;
    std::size_t usedBytes_ = 0;
    std::size_t budget_ = kDefaultBudget;
    std::uint32_t nextQuery_ = 1;
    mutable std::uint64_t useClock_ = 1;
    std::uint64_t pumps_ = 0;
    TileStore* store_ = nullptr;
    bool storeResolved_ = false;
    std::size_t persistRestored_ = 0;
    std::size_t persistWritten_ = 0;

    static double nowMs();
    TileKey makeKey(std::uint32_t signalId, int level, std::int64_t index) const;
    void touch(const Tile& tile) const;
    void want(HistoryIO& io, std::uint32_t signalId, int level, std::int64_t index, bool urgent);
    void cursorTiles(HistoryIO& io, std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs, bool urgent);
    bool askDisk(const TileKey& key);
    void resolveStore();
    void accountTile(Tile& tile);
    void evictIfNeeded();
    void setSettledState(Tile& tile);
    void complete(std::uint32_t qid);
    void failRequest(std::uint32_t qid, double now);
    void queuePersist(const TileKey& key);
    void admitPersisted(const PersistedTile& p);
    std::uint64_t ringClipMs(std::uint32_t signalId) const;
};

}  // namespace photon::ui
