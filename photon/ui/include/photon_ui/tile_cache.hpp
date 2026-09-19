#pragma once

#include "photon/control.hpp"
#include "photon/protocol.hpp"
#include "photon/tsdb.hpp"
#include "photon_ui/history_io.hpp"
#include "photon_ui/tile_store.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace photon::ui {

enum class TileState : std::uint8_t { Missing, Requested, Open, Sealed, Empty };

struct TileKey {
    std::string sessionId;
    std::string dbcHash;
    std::uint32_t signalId = 0;
    std::uint8_t level = 0;
    std::int64_t tileIndex = 0;

    bool operator==(const TileKey& o) const {
        return signalId == o.signalId && level == o.level && tileIndex == o.tileIndex && sessionId == o.sessionId &&
               dbcHash == o.dbcHash;
    }
};

struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const {
        std::size_t h = k.signalId;
        h ^= static_cast<std::size_t>(k.level) << 16;
        h ^= static_cast<std::size_t>(k.tileIndex) * 2654435761u;
        h ^= std::hash<std::string>{}(k.sessionId);
        return h;
    }
};

struct Tile {
    TileKey key;
    TileState state = TileState::Missing;
    std::uint32_t queryId = 0;
    std::uint32_t bucketMs = 0;
    bool pinned = false;
    bool overview = false;
    // True once this tile's bytes are in the persistent store, whether they got
    // there by a write or by being read back from it. Read by persistable() to
    // keep a tile from being written on every frame it stays resident.
    bool persisted = false;
    std::vector<double> t;
    std::vector<float> minV;
    std::vector<float> maxV;
    std::vector<float> meanV;
    std::vector<float> rawV;
    mutable std::uint64_t lastUsed = 0;
    std::size_t bytes = 0;
    mutable Tile* lruPrev = nullptr;
    mutable Tile* lruNext = nullptr;
};

struct PlotArrays {
    std::vector<double> t;
    std::vector<double> y;
    std::vector<double> yMin;
    std::vector<double> yMax;
    bool envelope = false;
    bool incomplete = false;
};

class TileCache {
public:
    static constexpr std::size_t kDefaultBudget = 256 * 1024 * 1024;
    // A pending disk read defers the network request for the tiles it covers, so
    // it must not be able to defer forever if the bridge never answers.
    static constexpr std::uint64_t kDiskDeadlinePumps = 240;
    static constexpr std::size_t kMaxDiskPending = 32;
    static constexpr std::size_t kMaxDiskAskedIntervals = 16;
    static constexpr std::size_t kWriteQueueCap = 256;
    static constexpr std::size_t kWritesPerPump = 24;
    // A tile is only written once the session's write frontier has moved this
    // far past its end, mirroring the "tileEndMs < liveMs - bucketMs" rule.
    static constexpr std::uint64_t kSealMarginMs = 1000;

    // Overrides the process-wide default store. Passing nullptr disables
    // persistence entirely, which is the native default.
    void setTileStore(TileStore* store);

    void setSession(std::string sessionId, std::string dbcHash, std::uint64_t minMs, std::uint64_t maxMs);
    void updateRingOldest(std::uint32_t signalId, std::uint64_t oldestMs);
    void clearRingOldest();
    void cancelAll(HistoryIO& client);
    std::uint64_t seamFor(std::uint32_t signalId) const;
    void setPlayback(double speed, bool playing);
    void setCompactFetch(bool on) { compactFetch_ = on; }

    void requestWindow(HistoryIO& client, std::span<const std::uint32_t> signalIds, std::uint64_t fromMs,
                       std::uint64_t toMs, std::uint32_t maxPoints, bool dragging);
    void requestCursorWindow(HistoryIO& client, std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs);
    // Cold start: cursor L0 + one visible window. No neighbor / L±1 prefetch.
    void primeStartup(HistoryIO& client, std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs);
    void ingestResult(const ctrl::Message& msg);
    void ingestChunk(const proto::HistoryChunk& chunk);
    void cancelLeaving(HistoryIO& client, std::uint64_t fromMs, std::uint64_t toMs);

    bool sampleAt(std::uint32_t signalId, std::uint64_t tMs, double& value) const;
    // Nearest L0 sample within slackMs (before or after). Catalog pins start at
    // session minMs, which is often a few seconds before the first GPS fix.
    bool sampleNear(std::uint32_t signalId, std::uint64_t tMs, double& value,
                    std::uint64_t slackMs = 120000) const;
    // First |value| >= minAbs in any resident tile. Used when the playhead sits
    // on logged 0,0 before the GPS receiver has a fix.
    bool sampleFirstAbs(std::uint32_t signalId, double& value, double minAbs) const;
    PlotArrays gatherPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                          std::uint32_t maxPoints = 0) const;
    // L0/raw only. Never min/max/mean. Used for threshold crossings (A5).
    PlotArrays gatherL0(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const;
    void stitchRing(PlotArrays& dest, std::span<const double> t, std::span<const double> v, std::size_t count,
                    std::size_t cap, std::size_t write, std::uint64_t fromMs, std::uint64_t toMs,
                    std::uint32_t signalId) const;

    std::uint32_t prepareQuery(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                               std::uint8_t guessedLevel, bool pin, bool overview);
    void bindQuery(std::uint32_t queryId, std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                   std::uint8_t guessedLevel);

    // Analysis-owned caches start query ids in a reserved high range so they
    // never collide with ClientArena::tiles_ (C3). 0 keeps the live default (1).
    void setQueryOrigin(std::uint32_t origin) { nextQuery_ = origin == 0 ? 1u : origin; }
    std::uint32_t nextQueryId() const { return nextQuery_; }
    bool hasInflight(std::uint32_t queryId) const { return inflight_.contains(queryId); }

    const std::string& sessionId() const { return sessionId_; }
    const std::string& dbcHash() const { return dbcHash_; }
    std::uint64_t sessionMinMs() const { return sessionMinMs_; }
    std::uint64_t sessionMaxMs() const { return sessionMaxMs_; }
    std::size_t overshootBytes() const { return overshootBytes_; }
    std::size_t usedBytes() const { return usedBytes_; }
    std::size_t boundBytes() const;
    std::size_t residentBytes() const;
    std::size_t budget() const { return budget_; }
    std::size_t tileCount() const { return tiles_.size(); }
    std::size_t overviewRefuseCount() const { return overviewRefuseCount_; }
    void setBudget(std::size_t bytes) { budget_ = bytes == 0 ? kDefaultBudget : bytes; }
    bool hasOverview(std::uint32_t signalId) const { return overviewSignals_.contains(signalId); }

    std::size_t persistRestoredCount() const { return persistRestored_; }
    std::size_t persistRefusedCount() const { return persistRefused_; }
    std::size_t persistRejectedCount() const { return persistRejected_; }
    std::size_t persistWrittenCount() const { return persistWritten_; }
    std::size_t persistDroppedCount() const { return persistDropped_; }
    std::size_t persistPendingReads() const { return diskPending_.size(); }

private:
    struct InflightQuery {
        std::uint32_t signalId = 0;
        std::uint64_t fromMs = 0;
        std::uint64_t toMs = 0;
        std::uint32_t maxPoints = 0;
        std::uint8_t guessedLevel = 0;
        std::uint8_t servedLevel = 255;
        std::uint32_t expectedSeq = 0;
        ctrl::Message::HistoryPoint prior;
        ctrl::Message::HistoryPoint next;
        bool pin = false;
        bool overview = false;
    };

    struct PendingRange {
        std::uint32_t signalId = 0;
        std::uint8_t level = 0;
        std::int64_t indexFrom = 0;
        std::int64_t indexTo = 0;
        std::uint64_t deadlinePump = 0;
    };

    struct BoundSample {
        std::uint32_t signalId = 0;
        std::uint64_t t = 0;
        double v = 0;
        std::uint64_t fromMs = 0;
        std::uint64_t toMs = 0;
        bool prior = true;
    };

    std::string sessionId_;
    std::string dbcHash_;
    std::uint64_t sessionMinMs_ = 0;
    std::uint64_t sessionMaxMs_ = 0;
    mutable std::uint64_t clock_ = 1;
    std::size_t usedBytes_ = 0;
    std::size_t overshootBytes_ = 0;
    std::size_t budget_ = kDefaultBudget;
    std::uint32_t nextQuery_ = 1;
    double panVel_ = 0.0;
    std::uint64_t prevFromMs_ = 0;
    std::uint64_t prevToMs_ = 0;
    double playSpeed_ = 1.0;
    bool playing_ = false;
    bool compactFetch_ = false;
    mutable std::unordered_map<TileKey, Tile, TileKeyHash> tiles_;
    std::unordered_map<std::uint32_t, InflightQuery> inflight_;
    std::vector<BoundSample> bounds_;
    std::unordered_map<std::uint32_t, std::uint64_t> ringOldest_;
    std::unordered_set<std::uint32_t> overviewSignals_;
    std::vector<std::uint32_t> pendingCancels_;
    std::size_t overviewRefuseCount_ = 0;
    TileStore* store_ = nullptr;
    bool storeResolved_ = false;
    bool scopePushed_ = false;
    std::uint64_t pump_ = 0;
    std::size_t persistRestored_ = 0;
    std::size_t persistRefused_ = 0;
    std::size_t persistRejected_ = 0;
    std::size_t persistWritten_ = 0;
    std::size_t persistDropped_ = 0;
    std::vector<PendingRange> diskPending_;
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::int64_t, std::int64_t>>> diskAsked_;
    std::vector<TileKey> writeQueue_;
    std::vector<LoadedTileRange> loadedScratch_;
    mutable Tile* regularMru_ = nullptr;
    mutable Tile* regularLru_ = nullptr;
    mutable Tile* overviewMru_ = nullptr;
    mutable Tile* overviewLru_ = nullptr;

    TileKey makeKey(std::uint32_t signalId, int level, std::int64_t index) const;
    void evict(std::size_t roomNeeded, bool forOverview, HistoryIO* io);
    bool enqueue(HistoryIO& client, const TileKey& key, std::uint64_t fromMs, std::uint64_t toMs,
                 std::uint32_t maxPoints, bool pin, bool overview);
    void dropBoundOldest();
    void requestOverview(HistoryIO& client, std::span<const std::uint32_t> signalIds);
    void touch(const TileKey& key) const;
    void lruUnlink(Tile& tile) const;
    void lruTouch(Tile& tile) const;
    void flushCancels(HistoryIO& client);
    void noteCancel(std::uint32_t queryId, HistoryIO* io);
    void eraseTile(std::unordered_map<TileKey, Tile, TileKeyHash>::iterator it, HistoryIO* io);
    void repin(std::span<const std::uint32_t> signalIds, std::uint64_t fromMs, std::uint64_t toMs, int level);
    void accountTile(Tile& tile, std::size_t nextBytes);
    void markEmptyRange(std::uint32_t signalId, int level, std::uint64_t fromMs, std::uint64_t toMs,
                        std::uint32_t queryId);
    std::uint64_t contiguousSealedEnd(std::uint32_t signalId, std::uint64_t ringOldest) const;

    void pumpPersistence();
    void resolveStore();
    void resetPersistState();
    bool overlapsLiveRing(std::uint32_t signalId, int level, std::int64_t index) const;
    void demoteLiveOverlap(std::uint32_t signalId, std::uint64_t ringOldest);
    void askDisk(std::uint32_t signalId, int level, std::int64_t indexFrom, std::int64_t indexTo);
    bool diskReadPending(const TileKey& key) const;
    void admitPersisted(const PersistedTile& p, const LoadedTileRange& range);
    bool persistable(const Tile& tile) const;
    void queuePersist(const TileKey& key);
    void flushPersistWrites();
};

}  // namespace photon::ui
