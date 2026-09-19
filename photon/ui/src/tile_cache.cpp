#include "photon_ui/tile_cache.hpp"

#include "photon/interp.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace photon::ui {
namespace {

std::size_t tileBytes(const Tile& t) {
    return t.t.capacity() * sizeof(double) +
           (t.minV.capacity() + t.maxV.capacity() + t.meanV.capacity() + t.rawV.capacity()) * sizeof(float) +
           t.key.sessionId.capacity() + t.key.dbcHash.capacity() + 256;
}

std::size_t expectedTileBytes(int level) {
    if (level <= 0) {
        return 96 * 1024;
    }
    return static_cast<std::size_t>(tsdb::kRollupTileBuckets) * 16 + 256;
}

bool slotPresent(const proto::HistoryChunk& chunk, std::uint32_t i) {
    if ((chunk.flags & proto::kHistDense) != 0) {
        return true;
    }
    if (i >= chunk.slotCount) {
        return false;
    }
    const std::size_t byte = static_cast<std::size_t>(i) / 8;
    if (byte >= chunk.presence.size()) {
        return false;
    }
    return (chunk.presence[byte] & static_cast<std::uint8_t>(1u << (i % 8))) != 0;
}

bool tileUsable(const Tile& t) {
    return t.state == TileState::Sealed || t.state == TileState::Open || t.state == TileState::Empty || !t.t.empty();
}

void foldPlot(PlotArrays& a, std::uint32_t maxPoints) {
    if (maxPoints == 0 || a.t.size() <= maxPoints) {
        return;
    }
    if (!a.envelope) {
        a.yMin = a.y;
        a.yMax = a.y;
        a.envelope = true;
    }
    while (a.t.size() > maxPoints && a.t.size() >= 2) {
        PlotArrays next;
        next.envelope = true;
        next.t.reserve((a.t.size() + 1) / 2);
        next.y.reserve((a.t.size() + 1) / 2);
        next.yMin.reserve((a.t.size() + 1) / 2);
        next.yMax.reserve((a.t.size() + 1) / 2);
        for (std::size_t i = 0; i < a.t.size(); i += 2) {
            if (i + 1 >= a.t.size()) {
                next.t.push_back(a.t[i]);
                next.y.push_back(a.y[i]);
                next.yMin.push_back(i < a.yMin.size() ? a.yMin[i] : a.y[i]);
                next.yMax.push_back(i < a.yMax.size() ? a.yMax[i] : a.y[i]);
                break;
            }
            const double y0 = a.y[i];
            const double y1 = a.y[i + 1];
            const double mn0 = i < a.yMin.size() ? a.yMin[i] : y0;
            const double mn1 = i + 1 < a.yMin.size() ? a.yMin[i + 1] : y1;
            const double mx0 = i < a.yMax.size() ? a.yMax[i] : y0;
            const double mx1 = i + 1 < a.yMax.size() ? a.yMax[i + 1] : y1;
            next.t.push_back(a.t[i]);
            next.y.push_back((y0 + y1) * 0.5);
            next.yMin.push_back(std::min(mn0, mn1));
            next.yMax.push_back(std::max(mx0, mx1));
        }
        a = std::move(next);
    }
}

std::uint64_t diskAskedKey(std::uint32_t signalId, int level) {
    return (static_cast<std::uint64_t>(signalId) << 3) | static_cast<std::uint64_t>(level & 0x7);
}

void orderPlotByTime(PlotArrays& a) {
    const std::size_t n = std::min(a.t.size(), a.y.size());
    if (n == 0) {
        a.t.clear();
        a.y.clear();
        a.yMin.clear();
        a.yMax.clear();
        return;
    }
    const bool env = a.envelope && a.yMin.size() >= n && a.yMax.size() >= n;
    bool ordered = true;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(a.t[i]) || !std::isfinite(a.y[i])) {
            ordered = false;
            continue;
        }
        if (env && (!std::isfinite(a.yMin[i]) || !std::isfinite(a.yMax[i]))) {
            ordered = false;
            continue;
        }
        if (kept > 0 && a.t[i] < a.t[kept - 1]) {
            ordered = false;
        }
        ++kept;
    }
    if (ordered && kept == n) {
        return;
    }

    std::vector<std::size_t> idx;
    idx.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(a.t[i]) || !std::isfinite(a.y[i])) {
            continue;
        }
        if (env && (!std::isfinite(a.yMin[i]) || !std::isfinite(a.yMax[i]))) {
            continue;
        }
        idx.push_back(i);
    }
    if (idx.empty()) {
        return;
    }
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t i, std::size_t j) { return a.t[i] < a.t[j]; });

    PlotArrays next;
    next.envelope = env;
    next.incomplete = a.incomplete;
    next.t.reserve(idx.size());
    next.y.reserve(idx.size());
    if (env) {
        next.yMin.reserve(idx.size());
        next.yMax.reserve(idx.size());
    }
    for (std::size_t i = 0; i < idx.size(); ++i) {
        const auto k = idx[i];
        if (!next.t.empty() && a.t[k] <= next.t.back()) {
            continue;
        }
        next.t.push_back(a.t[k]);
        next.y.push_back(a.y[k]);
        if (env) {
            next.yMin.push_back(a.yMin[k]);
            next.yMax.push_back(a.yMax[k]);
        }
    }
    a = std::move(next);
}

void mergeIntervals(std::vector<std::pair<std::int64_t, std::int64_t>>& v) {
    if (v.size() < 2) {
        return;
    }
    std::sort(v.begin(), v.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    out.reserve(v.size());
    for (const auto& iv : v) {
        if (!out.empty() && iv.first <= out.back().second + 1) {
            out.back().second = std::max(out.back().second, iv.second);
            continue;
        }
        out.push_back(iv);
    }
    v = std::move(out);
}

}  // namespace

void TileCache::setTileStore(TileStore* store) {
    store_ = store;
    storeResolved_ = true;
    scopePushed_ = false;
    resetPersistState();
}

void TileCache::resolveStore() {
    if (storeResolved_) {
        return;
    }
    storeResolved_ = true;
    store_ = defaultTileStore();
}

void TileCache::resetPersistState() {
    diskPending_.clear();
    diskAsked_.clear();
    writeQueue_.clear();
    loadedScratch_.clear();
}

void TileCache::setSession(std::string sessionId, std::string dbcHash, std::uint64_t minMs, std::uint64_t maxMs) {
    if (sessionId_ != sessionId || dbcHash_ != dbcHash) {
        resetPersistState();
        scopePushed_ = false;
        tiles_.clear();
        inflight_.clear();
        bounds_.clear();
        ringOldest_.clear();
        overviewSignals_.clear();
        pendingCancels_.clear();
        usedBytes_ = 0;
        overshootBytes_ = 0;
        overviewRefuseCount_ = 0;
        regularMru_ = regularLru_ = nullptr;
        overviewMru_ = overviewLru_ = nullptr;
    }
    sessionId_ = std::move(sessionId);
    dbcHash_ = std::move(dbcHash);
    sessionMinMs_ = minMs;
    sessionMaxMs_ = maxMs;
    pumpPersistence();
}

void TileCache::updateRingOldest(std::uint32_t signalId, std::uint64_t oldestMs) {
    if (signalId == 0) {
        return;
    }
    if (oldestMs == 0) {
        ringOldest_.erase(signalId);
        return;
    }
    ringOldest_[signalId] = oldestMs;
    demoteLiveOverlap(signalId, oldestMs);
}

void TileCache::clearRingOldest() {
    ringOldest_.clear();
}

void TileCache::cancelAll(HistoryIO& client) {
    flushCancels(client);
    for (const auto& [id, q] : inflight_) {
        (void)q;
        if (id != 0) {
            client.cancelHistory(id);
        }
    }
    inflight_.clear();
    pendingCancels_.clear();
}

std::uint64_t TileCache::contiguousSealedEnd(std::uint32_t signalId, std::uint64_t ringOldest) const {
    if (ringOldest == 0) {
        return 0;
    }
    const auto span = tsdb::tileSpanMs(0);
    if (span == 0) {
        return 0;
    }
    auto i = tsdb::tileIndexOf(ringOldest > 0 ? ringOldest - 1 : 0, 0);
    std::uint64_t adjoiningEnd = 0;
    for (; i >= 0; --i) {
        const auto key = makeKey(signalId, 0, i);
        const auto it = tiles_.find(key);
        if (it == tiles_.end() || (it->second.state != TileState::Sealed && it->second.state != TileState::Empty)) {
            break;
        }
        if (adjoiningEnd == 0) {
            adjoiningEnd = static_cast<std::uint64_t>(i + 1) * span;
        }
        if (i == 0) {
            break;
        }
    }
    if (adjoiningEnd == 0) {
        return 0;
    }
    return std::min(ringOldest, adjoiningEnd);
}

std::size_t TileCache::boundBytes() const {
    return bounds_.size() * sizeof(BoundSample);
}

std::size_t TileCache::residentBytes() const {
    return usedBytes_ + boundBytes();
}

void TileCache::dropBoundOldest() {
    if (!bounds_.empty()) {
        bounds_.erase(bounds_.begin());
    }
}

std::uint64_t TileCache::seamFor(std::uint32_t signalId) const {
    const auto it = ringOldest_.find(signalId);
    const auto ring = it == ringOldest_.end() ? 0 : it->second;
    if (ring == 0) {
        return 0;
    }
    return contiguousSealedEnd(signalId, ring);
}

void TileCache::setPlayback(double speed, bool playing) {
    playSpeed_ = speed > 0.0 ? speed : 1.0;
    playing_ = playing;
}

TileKey TileCache::makeKey(std::uint32_t signalId, int level, std::int64_t index) const {
    TileKey k;
    k.sessionId = sessionId_;
    k.dbcHash = dbcHash_;
    k.signalId = signalId;
    k.level = static_cast<std::uint8_t>(level);
    k.tileIndex = index;
    return k;
}

void TileCache::lruUnlink(Tile& tile) const {
    Tile*& head = tile.overview ? overviewMru_ : regularMru_;
    Tile*& tail = tile.overview ? overviewLru_ : regularLru_;
    if (tile.lruPrev != nullptr) {
        tile.lruPrev->lruNext = tile.lruNext;
    } else if (head == &tile) {
        head = tile.lruNext;
    }
    if (tile.lruNext != nullptr) {
        tile.lruNext->lruPrev = tile.lruPrev;
    } else if (tail == &tile) {
        tail = tile.lruPrev;
    }
    tile.lruPrev = nullptr;
    tile.lruNext = nullptr;
}

void TileCache::lruTouch(Tile& tile) const {
    lruUnlink(tile);
    Tile*& head = tile.overview ? overviewMru_ : regularMru_;
    Tile*& tail = tile.overview ? overviewLru_ : regularLru_;
    tile.lruNext = head;
    tile.lruPrev = nullptr;
    if (head != nullptr) {
        head->lruPrev = &tile;
    }
    head = &tile;
    if (tail == nullptr) {
        tail = &tile;
    }
    tile.lastUsed = ++clock_;
}

void TileCache::touch(const TileKey& key) const {
    const auto it = tiles_.find(key);
    if (it != tiles_.end()) {
        lruTouch(it->second);
    }
}

void TileCache::accountTile(Tile& tile, std::size_t nextBytes) {
    if (usedBytes_ >= tile.bytes) {
        usedBytes_ -= tile.bytes;
    } else {
        usedBytes_ = 0;
    }
    tile.bytes = nextBytes;
    usedBytes_ += tile.bytes;
}

void TileCache::noteCancel(std::uint32_t queryId, HistoryIO* io) {
    if (queryId == 0) {
        return;
    }
    if (io != nullptr) {
        io->cancelHistory(queryId);
        return;
    }
    pendingCancels_.push_back(queryId);
}

void TileCache::flushCancels(HistoryIO& client) {
    for (const auto id : pendingCancels_) {
        client.cancelHistory(id);
    }
    pendingCancels_.clear();
}

void TileCache::eraseTile(std::unordered_map<TileKey, Tile, TileKeyHash>::iterator it, HistoryIO* io) {
    if (it == tiles_.end()) {
        return;
    }
    auto& tile = it->second;
    if (tile.overview) {
        overviewSignals_.erase(tile.key.signalId);
    }
    if (tile.state == TileState::Requested && tile.queryId != 0) {
        noteCancel(tile.queryId, io);
    }
    if (tile.queryId != 0) {
        inflight_.erase(tile.queryId);
    }
    if (usedBytes_ >= tile.bytes) {
        usedBytes_ -= tile.bytes;
    } else {
        usedBytes_ = 0;
    }
    lruUnlink(tile);
    tiles_.erase(it);
}

void TileCache::evict(std::size_t roomNeeded, bool forOverview, HistoryIO* io) {
    overshootBytes_ = 0;
    const auto limit = budget_;
    const auto ceiling = roomNeeded >= limit ? 0 : limit - roomNeeded;
    auto under = [&] { return residentBytes() <= ceiling; };

    while (!under() && !bounds_.empty()) {
        dropBoundOldest();
    }

    auto evictList = [&](bool overviewList) {
        Tile* unpin = overviewList ? overviewLru_ : regularLru_;
        for (Tile* t = unpin; t != nullptr && !under(); t = t->lruPrev) {
            t->pinned = false;
        }
        while (!under()) {
            Tile* tail = overviewList ? overviewLru_ : regularLru_;
            if (tail == nullptr) {
                break;
            }
            tail->pinned = false;
            const auto key = tail->key;
            eraseTile(tiles_.find(key), io);
        }
    };

    // Regular tiles go first so an overview reservation can always make room.
    evictList(false);
    if (forOverview && !under()) {
        evictList(true);
    }
    if (residentBytes() > limit) {
        overshootBytes_ = residentBytes() - limit;
    }
}

std::uint32_t TileCache::prepareQuery(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                                      std::uint8_t guessedLevel, bool pin, bool overview) {
    InflightQuery q;
    q.signalId = signalId;
    q.fromMs = fromMs;
    q.toMs = toMs;
    q.guessedLevel = guessedLevel;
    q.pin = pin;
    q.overview = overview;
    const auto id = nextQuery_++;
    inflight_[id] = q;
    return id;
}

void TileCache::bindQuery(std::uint32_t queryId, std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                          std::uint8_t guessedLevel) {
    InflightQuery q;
    q.signalId = signalId;
    q.fromMs = fromMs;
    q.toMs = toMs;
    q.guessedLevel = guessedLevel;
    inflight_[queryId] = q;
    if (queryId >= nextQuery_) {
        nextQuery_ = queryId + 1;
    }
}

bool TileCache::enqueue(HistoryIO& client, const TileKey& key, std::uint64_t fromMs, std::uint64_t toMs,
                        std::uint32_t maxPoints, bool pin, bool overview) {
    flushCancels(client);
    {
        auto it = tiles_.find(key);
        if (it != tiles_.end() && (it->second.state == TileState::Requested || it->second.state == TileState::Sealed ||
                                   it->second.state == TileState::Empty || it->second.state == TileState::Open)) {
            const bool wasOverview = it->second.overview;
            if (pin) {
                it->second.pinned = true;
            }
            if (overview) {
                if (!wasOverview) {
                    lruUnlink(it->second);
                    it->second.overview = true;
                }
                it->second.pinned = true;
            }
            lruTouch(it->second);
            return true;
        }
    }
    // Hold the socket request while a disk read that covers this tile is in
    // flight, so a cold start does not fetch the same tile twice. The overview
    // is exempt: §9.5's guarantee that something is always drawable must not
    // wait on IndexedDB.
    if (!overview && diskReadPending(key)) {
        return false;
    }
    if (inflight_.size() >= 16) {
        return false;
    }
    const auto reserve = expectedTileBytes(key.level);
    if (residentBytes() + reserve > budget_) {
        evict(reserve, overview, &client);
    }
    if (residentBytes() + reserve > budget_) {
        if (overview) {
            ++overviewRefuseCount_;
        }
        return false;
    }
    auto [it, inserted] = tiles_.try_emplace(key);
    it->second.key = key;
    it->second.state = TileState::Requested;
    it->second.queryId = prepareQuery(key.signalId, fromMs, toMs, key.level, pin, overview);
    it->second.pinned = pin || overview;
    it->second.overview = overview;
    accountTile(it->second, reserve);
    lruTouch(it->second);
    const auto qid = it->second.queryId;
    client.requestHistory(qid, sessionId_, fromMs, toMs, {key.signalId}, maxPoints);
    return true;
}

void TileCache::requestOverview(HistoryIO& client, std::span<const std::uint32_t> signalIds) {
    if (sessionMaxMs_ <= sessionMinMs_) {
        return;
    }
    constexpr std::uint64_t kUnixMs2020 = 1577836800000ull;
    if (sessionMinMs_ < kUnixMs2020 && sessionMaxMs_ >= kUnixMs2020) {
        return;
    }
    flushCancels(client);
    constexpr int kOverviewLevel = 5;
    const auto span = tsdb::tileSpanMs(kOverviewLevel);
    auto i0 = tsdb::tileIndexOf(sessionMinMs_, kOverviewLevel);
    const auto i1 = tsdb::tileIndexOf(sessionMaxMs_, kOverviewLevel);
    constexpr std::int64_t kMaxOverviewTiles = 4;
    if (i1 > i0 && (i1 - i0) > kMaxOverviewTiles) {
        i0 = i1 - kMaxOverviewTiles;
    }
    for (const auto id : signalIds) {
        if (id == 0 || overviewSignals_.contains(id)) {
            continue;
        }
        bool admitted = true;
        for (auto i = i0; i <= i1; ++i) {
            const auto tileFrom = static_cast<std::uint64_t>(i) * span;
            if (!enqueue(client, makeKey(id, kOverviewLevel, i), tileFrom, tileFrom + span, 2000, true, true)) {
                admitted = false;
                break;
            }
        }
        if (admitted) {
            overviewSignals_.insert(id);
        }
    }
}

void TileCache::repin(std::span<const std::uint32_t> signalIds, std::uint64_t fromMs, std::uint64_t toMs, int level) {
    for (auto& [k, t] : tiles_) {
        if (!t.overview) {
            t.pinned = false;
        }
    }
    const auto pinRange = [&](int L, std::uint64_t a, std::uint64_t b) {
        const auto span = tsdb::tileSpanMs(L);
        if (span == 0) {
            return;
        }
        const auto i0 = tsdb::tileIndexOf(a, L);
        const auto i1 = tsdb::tileIndexOf(b, L);
        for (const auto id : signalIds) {
            if (id == 0) {
                continue;
            }
            for (auto i = i0; i <= i1; ++i) {
                const auto key = makeKey(id, L, i);
                auto it = tiles_.find(key);
                if (it != tiles_.end()) {
                    it->second.pinned = true;
                }
            }
        }
    };
    pinRange(level, fromMs, toMs);
}

void TileCache::requestWindow(HistoryIO& client, std::span<const std::uint32_t> signalIds, std::uint64_t fromMs,
                              std::uint64_t toMs, std::uint32_t maxPoints, bool dragging) {
    if (signalIds.empty() || toMs < fromMs) {
        return;
    }
    // Plots used to ask for [0, unix-now] while a day switch was still
    // Buffering. That floods /ws and kills the WASM UI.
    constexpr std::uint64_t kUnixMs2020 = 1577836800000ull;
    constexpr std::uint64_t kMaxSpanMs = 48ull * 60ull * 60ull * 1000ull;
    if (fromMs < kUnixMs2020 && toMs >= kUnixMs2020) {
        return;
    }
    if (toMs - fromMs > kMaxSpanMs) {
        fromMs = toMs - kMaxSpanMs;
    }
    flushCancels(client);
    // Drain finished disk reads before deciding what to ask the socket for, so a
    // tile that came back from IndexedDB this frame suppresses its own request.
    pumpPersistence();
    if (dragging) {
        cancelLeaving(client, fromMs, toMs);
        return;
    }
    if (compactFetch_) {
        const std::uint32_t pts = std::max(16u, maxPoints);
        const auto rawEst = (toMs - fromMs) / 20;
        const int level = tsdb::pickLevel(fromMs, toMs, pts, rawEst, 0x3f);
        const auto lspan = tsdb::tileSpanMs(level);
        if (lspan == 0) {
            return;
        }
        const auto i0 = tsdb::tileIndexOf(fromMs, level);
        const auto i1 = tsdb::tileIndexOf(toMs, level);
        for (const auto id : signalIds) {
            if (id == 0) {
                continue;
            }
            for (auto i = i1; i >= i0; --i) {
                const auto tileFrom = static_cast<std::uint64_t>(i) * lspan;
                enqueue(client, makeKey(id, level, i), tileFrom, tileFrom + lspan, pts, true, false);
                if (i == i0) {
                    break;
                }
            }
        }
        return;
    }
    // Ask disk for the overview span before requesting it over the socket. The
    // overview enqueue is exempt from the disk deferral, so this can only ever
    // save a request -- it can never delay the one thing §9.5 requires to be
    // available immediately.
    if (sessionMaxMs_ > sessionMinMs_) {
        auto o0 = tsdb::tileIndexOf(sessionMinMs_, 5);
        const auto o1 = tsdb::tileIndexOf(sessionMaxMs_, 5);
        constexpr std::int64_t kMaxOverviewTiles = 4;
        if (o1 > o0 && (o1 - o0) > kMaxOverviewTiles) {
            o0 = o1 - kMaxOverviewTiles;
        }
        for (const auto id : signalIds) {
            askDisk(id, 5, o0, o1);
        }
    }
    requestOverview(client, signalIds);
    const std::uint32_t pts = std::max(16u, maxPoints);
    const auto rawEst = (toMs - fromMs) / 20;
    const int level = tsdb::pickLevel(fromMs, toMs, pts, rawEst, 0x3f);
    const std::uint64_t width = toMs > fromMs ? toMs - fromMs : 1;
    if (prevFromMs_ != 0) {
        const auto delta = static_cast<double>(static_cast<std::int64_t>(fromMs) - static_cast<std::int64_t>(prevFromMs_));
        panVel_ = panVel_ * 0.7 + delta * 0.3;
    }
    prevFromMs_ = fromMs;
    prevToMs_ = toMs;

    std::uint64_t extraLeft = width;
    std::uint64_t extraRight = width;
    if (panVel_ < -static_cast<double>(width) * 0.01) {
        extraLeft = width * 2;
    }
    if (panVel_ > static_cast<double>(width) * 0.01) {
        extraRight = width * 2;
    }
    const std::uint64_t prefFrom = fromMs > extraLeft ? fromMs - extraLeft : 0;
    const std::uint64_t prefTo = toMs + extraRight;

    auto enqueueRange = [&](int L, std::uint64_t a, std::uint64_t b, std::uint32_t budget, bool pin) {
        const auto lspan = tsdb::tileSpanMs(L);
        if (lspan == 0 || b < a) {
            return;
        }
        const auto i0 = tsdb::tileIndexOf(a, L);
        const auto i1 = tsdb::tileIndexOf(b, L);
        for (const auto id : signalIds) {
            if (id == 0) {
                continue;
            }
            for (auto i = i1; i >= i0; --i) {
                const auto tileFrom = static_cast<std::uint64_t>(i) * lspan;
                enqueue(client, makeKey(id, L, i), tileFrom, tileFrom + lspan, budget, pin, false);
                if (i == i0) {
                    break;
                }
            }
        }
    };

    const auto askDiskRange = [&](int L, std::uint64_t a, std::uint64_t b) {
        if (tsdb::tileSpanMs(L) == 0 || b < a) {
            return;
        }
        const auto i0 = tsdb::tileIndexOf(a, L);
        const auto i1 = tsdb::tileIndexOf(b, L);
        for (const auto id : signalIds) {
            askDisk(id, L, i0, i1);
        }
    };
    askDiskRange(level, prefFrom, prefTo);
    if (level + 1 <= 5) {
        askDiskRange(level + 1, fromMs, toMs);
    }
    if (level > 0) {
        askDiskRange(level - 1, fromMs, toMs);
    }

    enqueueRange(level, fromMs, toMs, pts, true);
    enqueueRange(level, prefFrom, fromMs > 0 ? fromMs - 1 : 0, pts, true);
    enqueueRange(level, toMs + 1, prefTo, pts, true);
    if (level + 1 <= 5) {
        enqueueRange(level + 1, fromMs, toMs, pts, false);
    }
    if (level > 0) {
        enqueueRange(level - 1, fromMs, toMs, pts, false);
    }
    if (playing_) {
        const auto look = static_cast<std::uint64_t>(std::max(2000.0, playSpeed_ * 1000.0));
        enqueueRange(level, toMs, toMs + look, pts, false);
    }
    repin(signalIds, prefFrom, prefTo, level);
    evict(0, false, &client);
}

void TileCache::requestCursorWindow(HistoryIO& client, std::span<const std::uint32_t> signalIds,
                                    std::uint64_t cursorMs) {
    if (cursorMs == 0) {
        cursorMs = sessionMaxMs_;
    }
    if (cursorMs == 0) {
        return;
    }
    flushCancels(client);
    pumpPersistence();
    const auto span = tsdb::tileSpanMs(0);
    const auto idx = tsdb::tileIndexOf(cursorMs, 0);
    for (const auto id : signalIds) {
        askDisk(id, 0, idx > 0 ? idx - 1 : 0, idx + 1);
    }
    const std::int64_t d0 = compactFetch_ ? 0 : -1;
    const std::int64_t d1 = compactFetch_ ? 0 : 1;
    for (const auto id : signalIds) {
        if (id == 0) {
            continue;
        }
        for (std::int64_t d = d0; d <= d1; ++d) {
            const auto i = idx + d;
            if (i < 0) {
                continue;
            }
            const auto from = static_cast<std::uint64_t>(i) * span;
            enqueue(client, makeKey(id, 0, i), from, from + span, 8000, true, false);
        }
    }
}

void TileCache::primeStartup(HistoryIO& client, std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs) {
    if (signalIds.empty()) {
        return;
    }
    if (cursorMs == 0) {
        cursorMs = sessionMaxMs_;
    }
    if (cursorMs == 0 && sessionMaxMs_ > sessionMinMs_) {
        cursorMs = sessionMaxMs_;
    }
    requestCursorWindow(client, signalIds, cursorMs);
    if (sessionMaxMs_ <= sessionMinMs_ && cursorMs == 0) {
        return;
    }
    const std::uint64_t toMs = cursorMs != 0 ? cursorMs : sessionMaxMs_;
    const std::uint64_t fromMs = toMs > 60000 ? toMs - 60000 : sessionMinMs_;
    if (toMs < fromMs) {
        return;
    }
    flushCancels(client);
    pumpPersistence();
    const auto pts = 800u;
    const auto rawEst = (toMs - fromMs) / 20;
    const int level = tsdb::pickLevel(fromMs, toMs, pts, rawEst, 0x3f);
    const auto lspan = tsdb::tileSpanMs(level);
    if (lspan == 0) {
        return;
    }
    const auto i0 = tsdb::tileIndexOf(fromMs, level);
    const auto i1 = tsdb::tileIndexOf(toMs, level);
    for (const auto id : signalIds) {
        if (id == 0) {
            continue;
        }
        for (auto i = i1; i >= i0; --i) {
            const auto tileFrom = static_cast<std::uint64_t>(i) * lspan;
            enqueue(client, makeKey(id, level, i), tileFrom, tileFrom + lspan, pts, true, false);
            if (i == i0) {
                break;
            }
        }
    }
}

void TileCache::markEmptyRange(std::uint32_t signalId, int level, std::uint64_t fromMs, std::uint64_t toMs,
                               std::uint32_t queryId) {
    const auto span = tsdb::tileSpanMs(level);
    if (span == 0 || toMs < fromMs) {
        return;
    }
    const auto i0 = tsdb::tileIndexOf(fromMs, level);
    const auto i1 = tsdb::tileIndexOf(toMs, level);
    for (auto i = i0; i <= i1; ++i) {
        const auto key = makeKey(signalId, level, i);
        auto& tile = tiles_[key];
        tile.key = key;
        tile.queryId = queryId;
        tile.state = TileState::Empty;
        accountTile(tile, 64);
        lruTouch(tile);
    }
}

void TileCache::ingestResult(const ctrl::Message& msg) {
    auto it = inflight_.find(msg.queryId);
    if (it == inflight_.end()) {
        return;
    }
    auto& q = it->second;
    bool empty = true;
    for (const auto& s : msg.historySeries) {
        if (s.signalId != q.signalId) {
            continue;
        }
        q.servedLevel = static_cast<std::uint8_t>(s.level);
        q.prior = s.prior;
        q.next = s.next;
        if (s.prior.present || s.next.present) {
            const auto addBound = [&](const ctrl::Message::HistoryPoint& p, bool prior) {
                if (!p.present) {
                    return;
                }
                if (residentBytes() + sizeof(BoundSample) > budget_) {
                    evict(sizeof(BoundSample), false, nullptr);
                }
                if (residentBytes() + sizeof(BoundSample) > budget_) {
                    dropBoundOldest();
                }
                if (residentBytes() + sizeof(BoundSample) > budget_) {
                    return;
                }
                bounds_.push_back(BoundSample{s.signalId, p.t, p.v, q.fromMs, q.toMs, prior});
            };
            addBound(s.prior, true);
            addBound(s.next, false);
        }
        if (s.points > 0) {
            empty = false;
        }
    }
    if (empty) {
        const int level = q.servedLevel == 255 ? q.guessedLevel : q.servedLevel;
        markEmptyRange(q.signalId, level, q.fromMs, q.toMs, msg.queryId);
        if (q.guessedLevel != level) {
            const auto guessed = makeKey(q.signalId, q.guessedLevel, tsdb::tileIndexOf(q.fromMs, q.guessedLevel));
            auto git = tiles_.find(guessed);
            if (git != tiles_.end() && git->second.queryId == msg.queryId &&
                git->second.state == TileState::Requested) {
                git->second.state = TileState::Empty;
                git->second.queryId = 0;
                eraseTile(git, nullptr);
            }
        }
        inflight_.erase(msg.queryId);
    }
}

void TileCache::ingestChunk(const proto::HistoryChunk& chunk) {
    auto it = inflight_.find(chunk.queryId);
    if (it == inflight_.end()) {
        return;
    }
    auto q = it->second;
    if (chunk.chunkSeq != q.expectedSeq) {
        // Gap: still file what arrived so the plot is not blank, then continue.
    }
    q.expectedSeq = chunk.chunkSeq + 1;
    q.servedLevel = chunk.level;
    it->second = q;

    const int level = chunk.level;
    const auto span = tsdb::tileSpanMs(level);
    const auto ring = ringOldest_.contains(q.signalId) ? ringOldest_[q.signalId] : 0;

    auto fileTile = [&](std::int64_t idx) -> Tile& {
        const auto key = makeKey(chunk.signalId, level, idx);
        auto& tile = tiles_[key];
        if (tile.bytes == 0 && tile.state == TileState::Missing) {
            accountTile(tile, expectedTileBytes(level));
        }
        const bool wasOverview = tile.overview;
        tile.key = key;
        tile.bucketMs = chunk.bucketMs;
        tile.queryId = chunk.queryId;
        tile.pinned = tile.pinned || q.pin;
        if (q.overview && !wasOverview) {
            lruUnlink(tile);
            tile.overview = true;
        } else {
            tile.overview = tile.overview || q.overview;
        }
        lruTouch(tile);
        return tile;
    };

    if (q.guessedLevel != level) {
        const auto guessedIdx = tsdb::tileIndexOf(q.fromMs, q.guessedLevel);
        const auto guessed = makeKey(q.signalId, q.guessedLevel, guessedIdx);
        auto git = tiles_.find(guessed);
        if (git != tiles_.end() && git->second.queryId == chunk.queryId && git->second.state == TileState::Requested &&
            git->second.t.empty()) {
            git->second.state = TileState::Empty;
            git->second.queryId = 0;
            eraseTile(git, nullptr);
        }
    }

    if (chunk.bucketMs == 0) {
        for (std::size_t i = 0; i < chunk.timestamps.size(); ++i) {
            const auto t = static_cast<std::uint64_t>(chunk.timestamps[i]);
            auto& tile = fileTile(tsdb::tileIndexOf(t, level));
            tile.t.push_back(chunk.timestamps[i]);
            if (i < chunk.rawV.size()) {
                tile.rawV.push_back(chunk.rawV[i]);
            }
        }
    } else {
        std::uint32_t vi = 0;
        for (std::uint32_t i = 0; i < chunk.slotCount; ++i) {
            if (!slotPresent(chunk, i)) {
                continue;
            }
            const auto t = chunk.baseMs + static_cast<std::uint64_t>(chunk.bucketMs) * i;
            auto& tile = fileTile(tsdb::tileIndexOf(t, level));
            tile.t.push_back(static_cast<double>(t));
            if (vi < chunk.minV.size()) {
                tile.minV.push_back(chunk.minV[vi]);
            }
            if (vi < chunk.maxV.size()) {
                tile.maxV.push_back(chunk.maxV[vi]);
            }
            if (vi < chunk.meanV.size()) {
                tile.meanV.push_back(chunk.meanV[vi]);
            }
            ++vi;
        }
    }

    const bool final = (chunk.flags & proto::kHistFinal) != 0;
    const auto i0 = span == 0 ? 0 : tsdb::tileIndexOf(q.fromMs, level);
    const auto i1 = span == 0 ? 0 : tsdb::tileIndexOf(q.toMs, level);
    for (auto i = i0; i <= i1; ++i) {
        const auto key = makeKey(chunk.signalId, level, i);
        auto tit = tiles_.find(key);
        if (tit == tiles_.end() || tit->second.queryId != chunk.queryId) {
            continue;
        }
        auto& tile = tit->second;
        accountTile(tile, tileBytes(tile));
        if (residentBytes() > budget_) {
            evict(residentBytes() - budget_, false, nullptr);
        }
        if (!final) {
            if (tile.state != TileState::Sealed && tile.state != TileState::Empty) {
                tile.state = TileState::Requested;
            }
            continue;
        }
        const auto tileFrom = static_cast<std::uint64_t>(i) * span;
        const auto tileEnd = tileFrom + span;
        const bool overlapsLive = ring != 0 && tileEnd > ring;
        if (tile.t.empty()) {
            tile.state = TileState::Empty;
        } else if (!overlapsLive && ring != 0) {
            tile.state = TileState::Sealed;
        } else {
            tile.state = TileState::Open;
        }
        if (persistable(tile)) {
            queuePersist(key);
        }
    }
    if (final) {
        inflight_.erase(chunk.queryId);
    }
    evict(0, false, nullptr);
}

void TileCache::cancelLeaving(HistoryIO& client, std::uint64_t fromMs, std::uint64_t toMs) {
    flushCancels(client);
    // Applying finished reads during a drag is not a request, so it stays inside
    // §9.8's "a scrub sends nothing to the backend". No *new* read is started.
    pumpPersistence();
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        auto& tile = it->second;
        if (tile.state != TileState::Requested) {
            ++it;
            continue;
        }
        const auto span = tsdb::tileSpanMs(it->first.level);
        const auto tileFrom = static_cast<std::uint64_t>(it->first.tileIndex) * span;
        const auto tileTo = tileFrom + span;
        if (tileTo < fromMs || tileFrom > toMs) {
            auto next = std::next(it);
            eraseTile(it, &client);
            it = next;
            continue;
        }
        ++it;
    }
}

bool TileCache::sampleAt(std::uint32_t signalId, std::uint64_t tMs, double& value) const {
    const auto idx = tsdb::tileIndexOf(tMs, 0);
    std::optional<double> found;
    std::uint64_t foundT = 0;
    for (std::int64_t d = 0; d >= -1; --d) {
        const auto i = idx + d;
        if (i < 0) {
            continue;
        }
        const auto key = TileKey{sessionId_, dbcHash_, signalId, 0, i};
        const auto it = tiles_.find(key);
        if (it == tiles_.end() || it->second.t.empty()) {
            continue;
        }
        touch(key);
        const auto& tile = it->second;
        for (std::size_t k = 0; k < tile.t.size(); ++k) {
            const auto ts = static_cast<std::uint64_t>(tile.t[k]);
            if (ts <= tMs && (!found || ts >= foundT)) {
                const float v = k < tile.rawV.size() ? tile.rawV[k] : (k < tile.meanV.size() ? tile.meanV[k] : 0.0f);
                found = static_cast<double>(v);
                foundT = ts;
            }
        }
    }
    std::optional<double> boundV;
    std::uint64_t boundDist = ~0ull;
    for (const auto& b : bounds_) {
        if (b.signalId != signalId) {
            continue;
        }
        if (tMs < b.fromMs || tMs > b.toMs) {
            continue;
        }
        if (b.prior && b.t > tMs) {
            continue;
        }
        if (!b.prior && b.t < tMs) {
            continue;
        }
        const auto dist = b.t > tMs ? b.t - tMs : tMs - b.t;
        if (dist < boundDist) {
            boundDist = dist;
            boundV = b.v;
        }
    }
    if (found && (!boundV || (tMs - foundT) <= boundDist)) {
        value = *found;
        return true;
    }
    if (boundV) {
        value = *boundV;
        return true;
    }
    return false;
}

bool TileCache::sampleNear(std::uint32_t signalId, std::uint64_t tMs, double& value, std::uint64_t slackMs) const {
    if (sampleAt(signalId, tMs, value)) {
        return true;
    }
    std::optional<double> found;
    std::uint64_t bestDist = ~0ull;
    auto consider = [&](std::uint64_t ts, double v) {
        const auto dist = ts > tMs ? ts - tMs : tMs - ts;
        if (dist > slackMs || dist >= bestDist) {
            return;
        }
        found = v;
        bestDist = dist;
    };
    for (const auto& [key, tile] : tiles_) {
        if (key.signalId != signalId || tile.t.empty()) {
            continue;
        }
        for (std::size_t k = 0; k < tile.t.size(); ++k) {
            const auto ts = static_cast<std::uint64_t>(tile.t[k]);
            const float v = k < tile.rawV.size() ? tile.rawV[k]
                                                 : (k < tile.meanV.size() ? tile.meanV[k] : 0.0f);
            consider(ts, static_cast<double>(v));
        }
    }
    for (const auto& b : bounds_) {
        if (b.signalId == signalId) {
            consider(b.t, b.v);
        }
    }
    if (!found) {
        return false;
    }
    value = *found;
    return true;
}

bool TileCache::sampleFirstAbs(std::uint32_t signalId, double& value, double minAbs) const {
    for (const auto& [key, tile] : tiles_) {
        if (key.signalId != signalId || tile.t.empty()) {
            continue;
        }
        for (std::size_t k = 0; k < tile.t.size(); ++k) {
            const float v = k < tile.rawV.size() ? tile.rawV[k]
                                                 : (k < tile.meanV.size() ? tile.meanV[k] : 0.0f);
            if (std::fabs(static_cast<double>(v)) >= minAbs) {
                value = static_cast<double>(v);
                return true;
            }
        }
    }
    for (const auto& b : bounds_) {
        if (b.signalId == signalId && std::fabs(b.v) >= minAbs) {
            value = b.v;
            return true;
        }
    }
    return false;
}

PlotArrays TileCache::gatherPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                                 std::uint32_t maxPoints) const {
    PlotArrays out;
    if (toMs < fromMs) {
        return out;
    }
    const auto seam = seamFor(signalId);
    const auto ringIt = ringOldest_.find(signalId);
    const auto ringOldest = ringIt == ringOldest_.end() ? 0 : ringIt->second;
    // Sealed seam is 0 until a Sealed/Empty tile adjoins the ring, but Open
    // tiles still overlap it. stitchRing appends the ring after this array, so
    // those overlapping samples draw a line from window-end back to window-start.
    const auto clipMs = seam != 0 ? seam : ringOldest;
    if (clipMs != 0 && clipMs <= fromMs) {
        return out;
    }
    const auto histTo = clipMs != 0 ? std::min(toMs, clipMs - 1) : toMs;
    std::uint64_t cursor = fromMs;
    auto pushRaw = [&](double t, double v) {
        out.t.push_back(t);
        out.y.push_back(v);
        if (out.envelope) {
            out.yMin.push_back(v);
            out.yMax.push_back(v);
        }
    };
    auto pushEnv = [&](double t, double y, double mn, double mx) {
        if (!out.envelope && !out.t.empty()) {
            out.yMin = out.y;
            out.yMax = out.y;
        }
        out.envelope = true;
        out.t.push_back(t);
        out.y.push_back(y);
        out.yMin.push_back(mn);
        out.yMax.push_back(mx);
    };

    while (cursor <= histTo) {
        int used = -1;
        const Tile* tile = nullptr;
        std::uint64_t coverEnd = cursor + 1;
        for (int L = 0; L <= 5; ++L) {
            const auto idx = tsdb::tileIndexOf(cursor, L);
            const auto key = makeKey(signalId, L, idx);
            const auto it = tiles_.find(key);
            if (it != tiles_.end() && tileUsable(it->second)) {
                used = L;
                tile = &it->second;
                const auto span = tsdb::tileSpanMs(L);
                coverEnd = std::min(histTo + 1, static_cast<std::uint64_t>(idx + 1) * span);
                if (coverEnd <= cursor) {
                    coverEnd = cursor + 1;
                }
                touch(key);
                break;
            }
        }
        if (used < 0) {
            const auto span = tsdb::tileSpanMs(0);
            coverEnd = static_cast<std::uint64_t>(tsdb::tileIndexOf(cursor, 0) + 1) * span;
            if (coverEnd <= cursor) {
                coverEnd = cursor + 1;
            }
            coverEnd = std::min(histTo + 1, coverEnd);
            out.incomplete = true;
            if (out.t.empty()) {
                const double lo = static_cast<double>(cursor);
                const double hi = static_cast<double>(coverEnd > cursor ? coverEnd - 1 : cursor);
                const double nan = std::numeric_limits<double>::quiet_NaN();
                pushEnv(lo, nan, nan, nan);
                if (hi > lo) {
                    pushEnv(hi, nan, nan, nan);
                }
            }
            cursor = coverEnd;
            continue;
        }
        if (tile != nullptr && !tile->t.empty()) {
            for (std::size_t k = 0; k < tile->t.size(); ++k) {
                if (tile->t[k] < static_cast<double>(cursor) || tile->t[k] >= static_cast<double>(coverEnd)) {
                    continue;
                }
                if (tile->t[k] < static_cast<double>(fromMs) || tile->t[k] > static_cast<double>(histTo)) {
                    continue;
                }
                if (clipMs != 0 && tile->t[k] >= static_cast<double>(clipMs)) {
                    continue;
                }
                if (used == 0) {
                    pushRaw(tile->t[k], k < tile->rawV.size() ? static_cast<double>(tile->rawV[k]) : 0.0);
                } else {
                    pushEnv(tile->t[k], k < tile->meanV.size() ? static_cast<double>(tile->meanV[k]) : 0.0,
                            k < tile->minV.size() ? static_cast<double>(tile->minV[k]) : 0.0,
                            k < tile->maxV.size() ? static_cast<double>(tile->maxV[k]) : 0.0);
                }
            }
        }
        cursor = coverEnd;
    }
    orderPlotByTime(out);
    foldPlot(out, maxPoints);
    return out;
}

PlotArrays TileCache::gatherL0(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const {
    PlotArrays out;
    if (toMs < fromMs || signalId == 0) {
        return out;
    }
    const auto span = tsdb::tileSpanMs(0);
    if (span == 0) {
        return out;
    }
    const auto i0 = tsdb::tileIndexOf(fromMs, 0);
    const auto i1 = tsdb::tileIndexOf(toMs, 0);
    for (auto i = i0; i <= i1; ++i) {
        const auto key = makeKey(signalId, 0, i);
        const auto it = tiles_.find(key);
        if (it == tiles_.end() || !tileUsable(it->second) || it->second.t.empty()) {
            out.incomplete = true;
            continue;
        }
        touch(key);
        const auto& tile = it->second;
        for (std::size_t k = 0; k < tile.t.size(); ++k) {
            if (tile.t[k] < static_cast<double>(fromMs) || tile.t[k] > static_cast<double>(toMs)) {
                continue;
            }
            if (k >= tile.rawV.size()) {
                out.incomplete = true;
                continue;
            }
            out.t.push_back(tile.t[k]);
            out.y.push_back(static_cast<double>(tile.rawV[k]));
        }
    }
    out.envelope = false;
    orderPlotByTime(out);
    return out;
}

void TileCache::stitchRing(PlotArrays& dest, std::span<const double> t, std::span<const double> v, std::size_t count,
                           std::size_t cap, std::size_t write, std::uint64_t fromMs, std::uint64_t toMs,
                           std::uint32_t signalId) const {
    if (count == 0 || t.empty() || v.empty()) {
        return;
    }
    const auto seamMs = seamFor(signalId);
    const auto ringIt = ringOldest_.find(signalId);
    const auto ringOldest = ringIt == ringOldest_.end() ? 0 : ringIt->second;
    const auto clipMs = seamMs != 0 ? seamMs : ringOldest;
    double lo = static_cast<double>(fromMs);
    if (clipMs != 0) {
        lo = std::max(lo, static_cast<double>(clipMs));
    }
    const double hi = static_cast<double>(toMs);
    double lastT = std::numeric_limits<double>::quiet_NaN();
    const std::size_t nDest = std::min(dest.t.size(), dest.y.size());
    for (std::size_t k = 0; k < nDest; ++k) {
        if (std::isfinite(dest.t[k]) && std::isfinite(dest.y[k]) &&
            (!std::isfinite(lastT) || dest.t[k] > lastT)) {
            lastT = dest.t[k];
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto idx = interp::ringIndex(i, count, cap, write);
        if (idx >= t.size() || idx >= v.size()) {
            continue;
        }
        if (t[idx] < lo || t[idx] > hi) {
            continue;
        }
        if (std::isfinite(lastT) && t[idx] <= lastT) {
            continue;
        }
        dest.t.push_back(t[idx]);
        dest.y.push_back(v[idx]);
        if (dest.envelope) {
            dest.yMin.push_back(v[idx]);
            dest.yMax.push_back(v[idx]);
        }
        lastT = t[idx];
    }
    orderPlotByTime(dest);
}

// --- IndexedDB tile persistence -------------------------------------------
//
// Seam safety is the whole reason this section is shaped the way it is.
// `contiguousSealedEnd` lets a Sealed or Empty tile adjoining the ring advance
// the seam, and everything at or above the seam is drawn from the ring instead
// of from history. So a restored tile that claims Sealed where the network path
// would have said Open moves the seam and hides live samples. Three rules
// follow, and each is enforced below rather than assumed:
//
//   1. `Empty` is never written. "The server found nothing here" is the one
//      tile state that is a function of *when* you asked -- a live tail can
//      fill a range that was empty a minute ago -- and it is also the state
//      that most cheaply advances the seam. It is cheap to re-establish over
//      the socket, so it stays out of the store entirely.
//   2. A tile is only written once the session's write frontier has moved
//      past its end (`persistable`), so its span cannot still grow.
//   3. A restored tile re-runs the ingest-time live-overlap test
//      (`overlapsLiveRing`) and lands as `Open`, not `Sealed`, when it
//      overlaps the ring. Because the ring can also be learned *after* the
//      restore, `updateRingOldest` re-applies the same test
//      (`demoteLiveOverlap`).

bool TileCache::overlapsLiveRing(std::uint32_t signalId, int level, std::int64_t index) const {
    const auto span = tsdb::tileSpanMs(level);
    if (span == 0 || index < 0) {
        return false;
    }
    const auto it = ringOldest_.find(signalId);
    const auto ring = it == ringOldest_.end() ? 0 : it->second;
    if (ring == 0) {
        return false;
    }
    return static_cast<std::uint64_t>(index + 1) * span > ring;
}

void TileCache::demoteLiveOverlap(std::uint32_t signalId, std::uint64_t ringOldest) {
    if (ringOldest == 0) {
        return;
    }
    // Only the tile adjoining the ring can move the seam, so this is six map
    // lookups rather than a scan of the whole cache.
    for (int level = 0; level <= 5; ++level) {
        const auto idx = tsdb::tileIndexOf(ringOldest - 1, level);
        auto it = tiles_.find(makeKey(signalId, level, idx));
        if (it == tiles_.end() || it->second.state != TileState::Sealed) {
            continue;
        }
        if (overlapsLiveRing(signalId, level, idx)) {
            it->second.state = TileState::Open;
        }
    }
}

bool TileCache::persistable(const Tile& tile) const {
    if (tile.persisted) {
        return false;
    }
    // Empty is deliberately excluded (see rule 1 above). Requested/Missing mean
    // chunks are still arriving. Both are re-checked here rather than only at the
    // queueing site, because a tile's state can change between the two.
    if (tile.state != TileState::Sealed && tile.state != TileState::Open) {
        return false;
    }
    // Redundant with the state check on every path reachable today; kept so a
    // zero-sample record can never be written.
    if (tile.t.empty()) {
        return false;
    }
    const auto span = tsdb::tileSpanMs(tile.key.level);
    if (span == 0 || tile.key.tileIndex < 0) {
        return false;
    }
    const auto end = static_cast<std::uint64_t>(tile.key.tileIndex + 1) * span;
    if (sessionMaxMs_ == 0 || end + kSealMarginMs > sessionMaxMs_) {
        return false;
    }
    return !overlapsLiveRing(tile.key.signalId, tile.key.level, tile.key.tileIndex);
}

void TileCache::queuePersist(const TileKey& key) {
    if (writeQueue_.size() >= kWriteQueueCap) {
        ++persistDropped_;
        return;
    }
    for (const auto& queued : writeQueue_) {
        if (queued == key) {
            return;
        }
    }
    writeQueue_.push_back(key);
}

void TileCache::flushPersistWrites() {
    if (store_ == nullptr || writeQueue_.empty()) {
        return;
    }
    if (store_->storedBytes() > store_->byteBudget()) {
        // The store is over its own cap and prunes on write. Shed the backlog
        // rather than letting it grow while pruning catches up.
        persistDropped_ += writeQueue_.size();
        writeQueue_.clear();
        return;
    }
    std::size_t done = 0;
    while (!writeQueue_.empty() && done < kWritesPerPump) {
        const auto key = writeQueue_.back();
        writeQueue_.pop_back();
        ++done;
        auto it = tiles_.find(key);
        if (it == tiles_.end() || !persistable(it->second)) {
            continue;
        }
        const auto& tile = it->second;
        PersistedTile p;
        p.formatVersion = kTileBlobVersion;
        p.sessionId = sessionId_;
        p.dbcHash = dbcHash_;
        p.signalId = key.signalId;
        p.level = key.level;
        p.tileIndex = key.tileIndex;
        p.bucketMs = tile.bucketMs;
        p.t = tile.t;
        p.minV = tile.minV;
        p.maxV = tile.maxV;
        p.meanV = tile.meanV;
        p.rawV = tile.rawV;
        const auto blob = encodeTile(p);
        if (blob.size() > kTileBlobMaxBytes) {
            ++persistDropped_;
            continue;
        }
        const PersistedTileKey pk{key.signalId, key.level, key.tileIndex};
        if (store_->put(pk, blob)) {
            it->second.persisted = true;
            ++persistWritten_;
        } else {
            ++persistDropped_;
        }
    }
}

bool TileCache::diskReadPending(const TileKey& key) const {
    for (const auto& p : diskPending_) {
        if (p.signalId == key.signalId && p.level == key.level && key.tileIndex >= p.indexFrom &&
            key.tileIndex <= p.indexTo) {
            return true;
        }
    }
    return false;
}

void TileCache::askDisk(std::uint32_t signalId, int level, std::int64_t indexFrom, std::int64_t indexTo) {
    if (store_ == nullptr || signalId == 0 || level < 0 || level > 5 || indexTo < indexFrom || indexTo < 0) {
        return;
    }
    if (indexFrom < 0) {
        indexFrom = 0;
    }
    auto& asked = diskAsked_[diskAskedKey(signalId, level)];
    std::vector<std::pair<std::int64_t, std::int64_t>> gaps;
    std::int64_t cursor = indexFrom;
    for (const auto& iv : asked) {
        if (iv.second < cursor) {
            continue;
        }
        if (iv.first > indexTo) {
            break;
        }
        if (iv.first > cursor) {
            gaps.emplace_back(cursor, std::min(indexTo, iv.first - 1));
        }
        cursor = std::max(cursor, iv.second + 1);
        if (cursor > indexTo) {
            break;
        }
    }
    if (cursor <= indexTo) {
        gaps.emplace_back(cursor, indexTo);
    }
    for (const auto& gap : gaps) {
        if (diskPending_.size() >= kMaxDiskPending) {
            break;
        }
        if (!store_->requestRange(signalId, static_cast<std::uint8_t>(level), gap.first, gap.second)) {
            // Not accepted, so it must not be recorded as asked or it would
            // never be read and the tile would silently fall to the network
            // forever.
            continue;
        }
        diskPending_.push_back(PendingRange{signalId, static_cast<std::uint8_t>(level), gap.first, gap.second,
                                            pump_ + kDiskDeadlinePumps});
        asked.push_back(gap);
    }
    mergeIntervals(asked);
    if (asked.size() > kMaxDiskAskedIntervals) {
        // Collapsing over-suppresses: an interior index never actually asked
        // now counts as asked and falls to the network. That is the safe
        // direction, and it keeps this map bounded.
        const auto lo = asked.front().first;
        std::int64_t hi = lo;
        for (const auto& iv : asked) {
            hi = std::max(hi, iv.second);
        }
        asked.assign(1, {lo, hi});
    }
}

void TileCache::admitPersisted(const PersistedTile& p, const LoadedTileRange& range) {
    // Every one of these is a real invalidation path, not defensive noise: the
    // scope check is what makes a stale DBC or session a miss, and the range
    // check is what stops a bridge that returns the wrong keys from filing a
    // tile under a time it does not cover.
    if (p.formatVersion != kTileBlobVersion || p.sessionId != sessionId_ || p.dbcHash != dbcHash_) {
        ++persistRejected_;
        return;
    }
    if (p.signalId != range.signalId || p.level != range.level || p.tileIndex < range.indexFrom ||
        p.tileIndex > range.indexTo) {
        ++persistRejected_;
        return;
    }
    if (p.t.empty() || tsdb::tileSpanMs(p.level) == 0) {
        ++persistRejected_;
        return;
    }
    const auto key = makeKey(p.signalId, p.level, p.tileIndex);
    const auto existing = tiles_.find(key);
    if (existing != tiles_.end() && existing->second.state != TileState::Missing) {
        // Anything already known from the socket wins; a restore never
        // overwrites live-path state.
        return;
    }
    const auto reserve = expectedTileBytes(p.level);
    if (residentBytes() + reserve > budget_) {
        evict(reserve, false, nullptr);
    }
    if (residentBytes() + reserve > budget_) {
        // The in-memory cap stays hard. A refused restore is simply dropped;
        // the store is a cache of a cache.
        ++persistRefused_;
        return;
    }
    auto [it, inserted] = tiles_.try_emplace(key);
    auto& tile = it->second;
    tile.key = key;
    tile.bucketMs = p.bucketMs;
    tile.queryId = 0;
    tile.persisted = true;
    tile.t = p.t;
    tile.minV = p.minV;
    tile.maxV = p.maxV;
    tile.meanV = p.meanV;
    tile.rawV = p.rawV;
    tile.state = overlapsLiveRing(p.signalId, p.level, p.tileIndex) ? TileState::Open : TileState::Sealed;
    accountTile(tile, tileBytes(tile));
    if (residentBytes() > budget_) {
        evict(residentBytes() - budget_, false, nullptr);
    }
    if (residentBytes() > budget_) {
        // expectedTileBytes reserved the slot; a large L0 tile can still land
        // over the hard cap. Drop it rather than publish an overshoot.
        ++persistRefused_;
        eraseTile(it, nullptr);
        return;
    }
    lruTouch(tile);
    ++persistRestored_;
}

void TileCache::pumpPersistence() {
    resolveStore();
    if (store_ == nullptr) {
        return;
    }
    ++pump_;
    if (!scopePushed_ && !sessionId_.empty()) {
        store_->setScope(sessionId_, dbcHash_);
        scopePushed_ = true;
    }

    loadedScratch_.clear();
    store_->takeCompleted(loadedScratch_);
    for (const auto& range : loadedScratch_) {
        for (std::size_t i = 0; i < diskPending_.size();) {
            const auto& pend = diskPending_[i];
            if (pend.signalId == range.signalId && pend.level == range.level && pend.indexFrom == range.indexFrom &&
                pend.indexTo == range.indexTo) {
                diskPending_.erase(diskPending_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
        for (const auto& blob : range.blobs) {
            PersistedTile p;
            if (!decodeTile(blob, p)) {
                ++persistRejected_;
                continue;
            }
            admitPersisted(p, range);
        }
    }
    loadedScratch_.clear();

    // A bridge that never answers must not defer the network forever.
    for (std::size_t i = 0; i < diskPending_.size();) {
        if (pump_ >= diskPending_[i].deadlinePump) {
            diskPending_.erase(diskPending_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }

    flushPersistWrites();
}

}  // namespace photon::ui
