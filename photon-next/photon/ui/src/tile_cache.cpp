#include "photon_ui/tile_cache.hpp"

#include "photon/interp.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace photon::ui {
namespace {

double steadyNowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

TileCache::Clock& clockSlot() {
    static TileCache::Clock slot = &steadyNowMs;
    return slot;
}

// Pushes one plot point. A point of an envelope (level > 0) turns the arrays into
// envelope arrays; a raw point in an envelope array repeats its value as min and max.
void pushPoint(PlotArrays& out, double t, double y, double mn, double mx, bool env) {
    if (env && !out.envelope) {
        out.envelope = true;
        out.yMin = out.y;
        out.yMax = out.y;
    }
    out.t.push_back(t);
    out.y.push_back(y);
    if (out.envelope) {
        out.yMin.push_back(mn);
        out.yMax.push_back(mx);
    }
}

// Where the last sample of a gather came from, so a line breaks only between two
// known samples of the same level that are too far apart.
struct GapState {
    bool have = false;
    double t = 0.0;
    int level = -1;
};

double medianSpacing(const std::vector<double>& t) {
    if (t.size() < 3) {
        return 0.0;
    }
    std::vector<double> d;
    d.reserve(t.size() - 1);
    for (std::size_t i = 1; i < t.size(); ++i) {
        d.push_back(t[i] - t[i - 1]);
    }
    const auto mid = d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2);
    std::nth_element(d.begin(), mid, d.end());
    return *mid;
}

// Appends one tile's samples within [lo, hi]. periodMs is the signal's period
// (0 unknown); bucketMs is the level's bucket width (0 for raw).
void appendTile(PlotArrays& out, const Tile& tile, int level, double lo, double hi, double periodMs,
                double bucketMs, GapState& gs) {
    const bool env = level > 0;
    // The period rule is for raw samples only: a bucket's neighbours are a bucket apart, not a period.
    double limit = 0.0;
    if (!env) {
        limit = periodMs > 0.0 ? 3.0 * periodMs : 3.0 * medianSpacing(tile.t);
    }
    for (std::size_t k = 0; k < tile.t.size(); ++k) {
        const double t = tile.t[k];
        if (t < lo || t > hi) {
            continue;
        }
        if (gs.have && gs.level == level) {
            const double dt = t - gs.t;
            const bool gap = (limit > 0.0 && dt > limit) || (env && dt > 2.0 * bucketMs);
            if (gap) {
                const double nan = std::numeric_limits<double>::quiet_NaN();
                pushPoint(out, gs.t + dt * 0.5, nan, nan, nan, env || out.envelope);
            }
        }
        if (env) {
            const double mean = k < tile.meanV.size() ? static_cast<double>(tile.meanV[k]) : 0.0;
            const double mn = k < tile.minV.size() ? static_cast<double>(tile.minV[k]) : mean;
            const double mx = k < tile.maxV.size() ? static_cast<double>(tile.maxV[k]) : mean;
            pushPoint(out, t, mean, mn, mx, true);
        } else {
            const double v = k < tile.rawV.size() ? static_cast<double>(tile.rawV[k]) : 0.0;
            pushPoint(out, t, v, v, v, out.envelope);
        }
        gs.have = true;
        gs.t = t;
        gs.level = level;
    }
}

// The last sample at or before t, or nothing.
std::optional<std::size_t> lastAtOrBefore(const std::vector<double>& t, double tMs) {
    const auto it = std::upper_bound(t.begin(), t.end(), tMs);
    if (it == t.begin()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(t.begin(), it) - 1);
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
        next.incomplete = a.incomplete;
        const std::size_t n = a.t.size();
        for (std::size_t i = 0; i < n;) {
            // A break (non-finite point) is kept as it is, and a point beside one is not
            // averaged with it: a fold never bridges a gap.
            const bool beside = !std::isfinite(a.y[i]) || (i + 1 < n && !std::isfinite(a.y[i + 1]));
            if (i + 1 >= n || beside) {
                next.t.push_back(a.t[i]);
                next.y.push_back(a.y[i]);
                next.yMin.push_back(a.yMin[i]);
                next.yMax.push_back(a.yMax[i]);
                i += 1;
                continue;
            }
            next.t.push_back(a.t[i]);
            next.y.push_back((a.y[i] + a.y[i + 1]) * 0.5);
            next.yMin.push_back(std::min(a.yMin[i], a.yMin[i + 1]));
            next.yMax.push_back(std::max(a.yMax[i], a.yMax[i + 1]));
            i += 2;
        }
        const bool shrunk = next.t.size() < a.t.size();
        a = std::move(next);
        if (!shrunk) {
            break;
        }
    }
}

void mergeRingLike(PlotArrays& dest, std::span<const double> t, std::span<const double> v, std::size_t count,
                   std::size_t cap, std::size_t write, double lo, double hi) {
    double lastT = std::numeric_limits<double>::quiet_NaN();
    const std::size_t nDest = std::min(dest.t.size(), dest.y.size());
    for (std::size_t k = 0; k < nDest; ++k) {
        if (std::isfinite(dest.t[k]) && std::isfinite(dest.y[k]) && (!std::isfinite(lastT) || dest.t[k] > lastT)) {
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
        pushPoint(dest, t[idx], v[idx], v[idx], v[idx], dest.envelope);
        lastT = t[idx];
    }
}

}  // namespace

void TileCache::setClock(Clock clock) {
    clockSlot() = clock != nullptr ? clock : &steadyNowMs;
}

double TileCache::nowMs() {
    return clockSlot()();
}

void TileCache::setTileStore(TileStore* store) {
    store_ = store;
    storeResolved_ = true;
    disk_.clear();
    writeQueue_.clear();
    if (store_ != nullptr && !runId_.empty()) {
        store_->setScope(runId_, rev_, sha_);
    }
}

void TileCache::resolveStore() {
    if (storeResolved_) {
        return;
    }
    storeResolved_ = true;
    store_ = defaultTileStore();
}

void TileCache::setLevels(std::vector<std::int64_t> bucketMs) {
    TileLevels next(std::move(bucketMs));
    bool same = next.count() == levels_.count();
    for (int level = 0; same && level < next.count(); ++level) {
        same = next.bucketMs(level) == levels_.bucketMs(level);
    }
    if (same) {
        return;
    }
    // Tile spans change with the levels, so nothing held is valid any more.
    levels_ = std::move(next);
    tiles_.clear();
    inflight_.clear();
    disk_.clear();
    writeQueue_.clear();
    usedBytes_ = 0;
}

void TileCache::setRun(const std::string& runId, std::int64_t rev, const std::string& sha, std::uint64_t minMs,
                       std::uint64_t maxMs) {
    minMs_ = minMs;
    maxMs_ = maxMs;
    if (runId_ == runId && rev_ == rev && sha_ == sha) {
        return;
    }
    runId_ = runId;
    rev_ = rev;
    sha_ = sha;
    tiles_.clear();
    inflight_.clear();
    disk_.clear();
    writeQueue_.clear();
    ringOldest_.clear();
    usedBytes_ = 0;
    resolveStore();
    if (store_ != nullptr && !runId_.empty()) {
        // A new rev or sha makes the store's old records unreachable and it sweeps them.
        store_->setScope(runId_, rev_, sha_);
    }
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
}

std::uint64_t TileCache::ringClipMs(std::uint32_t signalId) const {
    const auto it = ringOldest_.find(signalId);
    return it == ringOldest_.end() ? 0 : it->second;
}

TileKey TileCache::makeKey(std::uint32_t signalId, int level, std::int64_t index) const {
    TileKey k;
    k.runId = runId_;
    k.rev = rev_;
    k.sha = sha_;
    k.signalId = signalId;
    k.level = static_cast<std::uint8_t>(level);
    k.tileIndex = index;
    return k;
}

const Tile* TileCache::find(std::uint32_t signalId, int level, std::int64_t index) const {
    const auto it = tiles_.find(makeKey(signalId, level, index));
    return it == tiles_.end() ? nullptr : &it->second;
}

void TileCache::touch(const Tile& tile) const {
    tile.lastUsed = ++useClock_;
}

void TileCache::accountTile(Tile& tile) {
    const std::size_t bytes = tile.t.capacity() * sizeof(double) +
                              (tile.rawV.capacity() + tile.minV.capacity() + tile.maxV.capacity() +
                               tile.meanV.capacity()) *
                                  sizeof(float) +
                              sizeof(Tile) + 64;
    usedBytes_ = usedBytes_ - tile.bytes + bytes;
    tile.bytes = bytes;
}

void TileCache::evictIfNeeded() {
    if (usedBytes_ <= budget_) {
        return;
    }
    struct Candidate {
        TileKey key;
        std::uint64_t lastUsed = 0;
    };
    std::vector<Candidate> candidates;
    for (const auto& [key, tile] : tiles_) {
        if (tile.haveData && tile.state != TileState::Requested) {
            candidates.push_back(Candidate{key, tile.lastUsed});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.lastUsed < b.lastUsed; });
    const std::size_t target = budget_ - budget_ / 10;
    for (const auto& c : candidates) {
        if (usedBytes_ <= target) {
            break;
        }
        const auto it = tiles_.find(c.key);
        if (it == tiles_.end()) {
            continue;
        }
        usedBytes_ -= it->second.bytes;
        tiles_.erase(it);
    }
}

void TileCache::setSettledState(Tile& tile) {
    if (!tile.haveData) {
        tile.state = TileState::Unknown;
        return;
    }
    // Empty is a complete response with no samples, sealed or not; only a sealed tile with samples is Sealed.
    tile.state = tile.t.empty() ? TileState::Empty : (tile.sealed ? TileState::Sealed : TileState::Open);
}

bool TileCache::askDisk(const TileKey& key) {
    if (store_ == nullptr) {
        return false;
    }
    if (!store_->requestRange(key.signalId, key.level, key.tileIndex, key.tileIndex)) {
        return false;
    }
    disk_[key] = DiskRead{pumps_ + kDiskDeadlinePumps, true};
    return true;
}

void TileCache::want(HistoryIO& io, std::uint32_t signalId, int level, std::int64_t index, bool urgent) {
    if (signalId == 0 || !levels_.valid() || level < 0 || level > levels_.top() || index < 0 || runId_.empty()) {
        return;
    }
    const TileKey key = makeKey(signalId, level, index);
    const double now = nowMs();
    const auto it = tiles_.find(key);
    if (it != tiles_.end()) {
        Tile& tile = it->second;
        switch (tile.state) {
            case TileState::Requested:
                return;
            case TileState::Sealed:
                touch(tile);
                return;
            case TileState::Empty:
                if (tile.sealed) {
                    touch(tile);
                    return;
                }
                if (now - tile.lastRequestMs < kOpenRefreshMs) {
                    return;
                }
                break;
            case TileState::Open:
                if (now - tile.lastRequestMs < kOpenRefreshMs) {
                    return;
                }
                break;
            case TileState::Unknown:
                if (now < tile.retryAtMs) {
                    return;
                }
                break;
        }
    } else if (store_ != nullptr) {
        // A restored tile may be on disk. The store answers once; a miss falls
        // through to the network.
        const auto d = disk_.find(key);
        if (d != disk_.end()) {
            if (d->second.pending) {
                return;
            }
        } else if (askDisk(key)) {
            return;
        }
    }
    if (io.queueFull()) {
        return;
    }
    const std::uint32_t qid = nextQuery_++;
    if (qid == 0) {
        return;
    }
    Tile& tile = tiles_[key];
    tile.key = key;
    tile.state = TileState::Requested;
    tile.qid = qid;
    tile.lastRequestMs = now;
    InFlight f;
    f.key = key;
    f.startedMs = now;
    inflight_[qid] = std::move(f);
    io.requestTile(qid, runId_, signalId, level, index, urgent);
}

void TileCache::requestWindow(HistoryIO& io, std::span<const std::uint32_t> signalIds, std::uint64_t fromMs,
                              std::uint64_t toMs, std::uint32_t pixels, bool dragging) {
    if (dragging || !levels_.valid() || runId_.empty() || toMs < fromMs) {
        return;
    }
    const int level = levels_.chooseLevel(toMs - fromMs, pixels);
    const std::int64_t i1 = levels_.indexOf(level, toMs);
    std::int64_t i0 = levels_.indexOf(level, fromMs);
    if (i1 - i0 + 1 > kMaxTilesPerWindow) {
        i0 = i1 - kMaxTilesPerWindow + 1;
    }
    for (const std::uint32_t id : signalIds) {
        for (std::int64_t i = i1; i >= i0; --i) {
            want(io, id, level, i, true);
        }
    }
}

void TileCache::cursorTiles(HistoryIO& io, std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs,
                            bool urgent) {
    if (!levels_.valid() || runId_.empty()) {
        return;
    }
    const std::int64_t idx = levels_.indexOf(0, cursorMs);
    const double intoTile = static_cast<double>(cursorMs) - static_cast<double>(idx * levels_.spanMs(0));
    for (const std::uint32_t id : signalIds) {
        want(io, id, 0, idx, urgent);
        // The last sample before the cursor may sit in the tile before this one.
        if (intoTile < 2000.0 && idx > 0) {
            want(io, id, 0, idx - 1, urgent);
        }
    }
}

void TileCache::requestCursorWindow(HistoryIO& io, std::span<const std::uint32_t> signalIds,
                                    std::uint64_t cursorMs) {
    cursorTiles(io, signalIds, cursorMs, true);
}

void TileCache::primeStartup(HistoryIO& io, std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs) {
    cursorTiles(io, signalIds, cursorMs, false);
}

void TileCache::cancelLeaving(HistoryIO& io, std::uint64_t fromMs, std::uint64_t toMs) {
    if (!io.queueFull()) {
        return;
    }
    for (auto it = inflight_.begin(); it != inflight_.end();) {
        const TileKey& key = it->second.key;
        const double lo = static_cast<double>(key.tileIndex * levels_.spanMs(key.level));
        const double hi = lo + static_cast<double>(levels_.spanMs(key.level));
        const bool overlaps = hi > static_cast<double>(fromMs) && lo <= static_cast<double>(toMs);
        if (overlaps) {
            ++it;
            continue;
        }
        const std::uint32_t qid = it->first;
        io.cancelTile(qid);
        if (const auto tit = tiles_.find(key); tit != tiles_.end() && tit->second.qid == qid) {
            tit->second.qid = 0;
            if (tit->second.haveData) {
                setSettledState(tit->second);
            } else {
                usedBytes_ -= tit->second.bytes;
                tiles_.erase(tit);
            }
        }
        it = inflight_.erase(it);
    }
}

void TileCache::cancelAll(HistoryIO& io) {
    for (const auto& [qid, f] : inflight_) {
        (void)f;
        io.cancelTile(qid);
    }
    abandonRequests();
}

void TileCache::abandonRequests() {
    for (const auto& [qid, f] : inflight_) {
        (void)qid;
        if (const auto tit = tiles_.find(f.key); tit != tiles_.end() && tit->second.state == TileState::Requested) {
            tit->second.qid = 0;
            if (tit->second.haveData) {
                setSettledState(tit->second);
            } else {
                usedBytes_ -= tit->second.bytes;
                tiles_.erase(tit);
            }
        }
    }
    inflight_.clear();
}

void TileCache::noteGap(std::uint64_t fromMs, std::uint64_t toMs) {
    for (auto& [key, tile] : tiles_) {
        if (tile.state == TileState::Requested || tile.sealed) {
            continue;
        }
        const auto lo = static_cast<double>(key.tileIndex * levels_.spanMs(key.level));
        const auto hi = lo + static_cast<double>(levels_.spanMs(key.level));
        if (hi > static_cast<double>(fromMs) && lo <= static_cast<double>(toMs)) {
            tile.lastRequestMs = nowMs() - kOpenRefreshMs;
        }
    }
}

void TileCache::failRequest(std::uint32_t qid, double now) {
    const auto it = inflight_.find(qid);
    if (it == inflight_.end()) {
        return;
    }
    const TileKey key = it->second.key;
    inflight_.erase(it);
    const auto tit = tiles_.find(key);
    if (tit == tiles_.end() || tit->second.state != TileState::Requested || tit->second.qid != qid) {
        return;
    }
    Tile& tile = tit->second;
    tile.qid = 0;
    tile.state = TileState::Unknown;
    tile.failures += 1;
    // 1 s, 2 s, 4 s, ... up to 30 s. Unknown is not empty: the data a refresh
    // replaces stays usable until the retry lands.
    const double wait = std::min(kRetryMaxMs, kRetryFirstMs * std::pow(2.0, static_cast<double>(tile.failures - 1)));
    tile.retryAtMs = now + wait;
}

bool TileCache::ingestError(std::uint32_t qid) {
    if (!inflight_.contains(qid)) {
        return false;
    }
    failRequest(qid, nowMs());
    return true;
}

void TileCache::complete(std::uint32_t qid) {
    const auto it = inflight_.find(qid);
    if (it == inflight_.end()) {
        return;
    }
    InFlight f = std::move(it->second);
    inflight_.erase(it);
    const auto tit = tiles_.find(f.key);
    if (tit == tiles_.end() || tit->second.state != TileState::Requested || tit->second.qid != qid) {
        return;
    }
    Tile& tile = tit->second;
    tile.t = std::move(f.t);
    tile.rawV = std::move(f.rawV);
    tile.minV = std::move(f.minV);
    tile.maxV = std::move(f.maxV);
    tile.meanV = std::move(f.meanV);
    tile.sealed = f.sealed;
    tile.haveData = true;
    tile.qid = 0;
    tile.failures = 0;
    tile.retryAtMs = 0.0;
    setSettledState(tile);
    accountTile(tile);
    touch(tile);
    if (tile.state == TileState::Sealed) {
        queuePersist(f.key);
    }
    evictIfNeeded();
}

bool TileCache::ingestPiece(const proto2::TilePiece& p) {
    const auto it = inflight_.find(p.qid);
    if (it == inflight_.end()) {
        return false;
    }
    InFlight& f = it->second;
    const TileKey& key = f.key;
    const bool matches = p.signal + 1 == key.signalId && p.level == key.level && p.index == key.tileIndex;
    if (!matches || p.offset != f.filled) {
        // A piece for another tile, or a gap in the offsets: the tile cannot be assembled.
        failRequest(p.qid, nowMs());
        return true;
    }
    const std::size_t n = p.n;
    if (p.tMs.size() != n) {
        failRequest(p.qid, nowMs());
        return true;
    }
    if (key.level == 0) {
        if (p.value.size() != n) {
            failRequest(p.qid, nowMs());
            return true;
        }
        f.rawV.reserve(f.rawV.size() + n);
        for (const double v : p.value) {
            f.rawV.push_back(static_cast<float>(v));
        }
    } else {
        if (p.min.size() != n || p.max.size() != n || p.mean.size() != n) {
            failRequest(p.qid, nowMs());
            return true;
        }
        for (std::size_t i = 0; i < n; ++i) {
            f.minV.push_back(static_cast<float>(p.min[i]));
            f.maxV.push_back(static_cast<float>(p.max[i]));
            f.meanV.push_back(static_cast<float>(p.mean[i]));
        }
    }
    f.t.insert(f.t.end(), p.tMs.begin(), p.tMs.end());
    f.filled += static_cast<std::uint32_t>(n);
    f.sealed = f.sealed || (p.flags & proto2::kTileSealed) != 0;
    if ((p.flags & proto2::kTileLast) != 0) {
        complete(p.qid);
    }
    return true;
}

bool TileCache::sampleAt(std::uint32_t signalId, std::uint64_t tMs, double& value) const {
    if (!levels_.valid() || signalId == 0) {
        return false;
    }
    const auto t = static_cast<double>(tMs);
    const std::int64_t idx = levels_.indexOf(0, t);
    const Tile* cur = find(signalId, 0, idx);
    if (cur != nullptr && cur->haveData) {
        touch(*cur);
        if (const auto k = lastAtOrBefore(cur->t, t)) {
            value = static_cast<double>(cur->rawV[*k]);
            return true;
        }
        if (idx > 0) {
            const Tile* prev = find(signalId, 0, idx - 1);
            if (prev != nullptr && prev->haveData && !prev->t.empty()) {
                value = static_cast<double>(prev->rawV[prev->t.size() - 1]);
                return true;
            }
        }
        return false;
    }
    // Raw is not here yet: the bucket of the finest coarser level that is, as its mean.
    for (int level = 1; level <= levels_.top(); ++level) {
        const Tile* tile = find(signalId, level, levels_.indexOf(level, t));
        if (tile == nullptr || !tile->haveData) {
            continue;
        }
        if (const auto k = lastAtOrBefore(tile->t, t)) {
            value = static_cast<double>(tile->meanV[*k]);
            return true;
        }
    }
    return false;
}

bool TileCache::sampleNear(std::uint32_t signalId, std::uint64_t tMs, double& value, std::uint64_t slackMs) const {
    if (!levels_.valid() || signalId == 0) {
        return false;
    }
    const auto t = static_cast<double>(tMs);
    const std::int64_t idx = levels_.indexOf(0, t);
    double best = std::numeric_limits<double>::infinity();
    bool found = false;
    for (std::int64_t i = idx - 1; i <= idx + 1; ++i) {
        if (i < 0) {
            continue;
        }
        const Tile* tile = find(signalId, 0, i);
        if (tile == nullptr || !tile->haveData || tile->t.empty()) {
            continue;
        }
        const auto pos = std::lower_bound(tile->t.begin(), tile->t.end(), t);
        const auto k = static_cast<std::size_t>(std::distance(tile->t.begin(), pos));
        for (const std::size_t c : {k, k == 0 ? k : k - 1}) {
            if (c >= tile->t.size()) {
                continue;
            }
            const double d = std::fabs(tile->t[c] - t);
            if (d < best && d <= static_cast<double>(slackMs)) {
                best = d;
                value = static_cast<double>(tile->rawV[c]);
                found = true;
            }
        }
    }
    return found;
}

bool TileCache::sampleFirstAbs(std::uint32_t signalId, double& value, double minAbs) const {
    std::optional<std::int64_t> firstIndex;
    const Tile* first = nullptr;
    for (const auto& [key, tile] : tiles_) {
        if (key.signalId != signalId || key.level != 0 || !tile.haveData) {
            continue;
        }
        for (std::size_t k = 0; k < tile.rawV.size(); ++k) {
            if (std::fabs(static_cast<double>(tile.rawV[k])) >= minAbs) {
                if (!firstIndex || key.tileIndex < *firstIndex) {
                    firstIndex = key.tileIndex;
                    first = &tile;
                    value = static_cast<double>(tile.rawV[k]);
                }
                break;
            }
        }
    }
    return first != nullptr;
}

PlotArrays TileCache::gatherPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                                 std::uint32_t maxPoints) const {
    PlotArrays out;
    if (toMs < fromMs || signalId == 0 || !levels_.valid()) {
        return out;
    }
    // The live ring draws from its oldest sample on, so tiles stop before it.
    const std::uint64_t clip = ringClipMs(signalId);
    if (clip != 0 && clip <= fromMs) {
        return out;
    }
    const std::uint64_t histTo = clip != 0 ? std::min(toMs, clip - 1) : toMs;
    const int level = levels_.chooseLevel(toMs - fromMs, maxPoints);
    const std::int64_t span = levels_.spanMs(level);
    const std::int64_t i0 = levels_.indexOf(level, fromMs);
    const std::int64_t i1 = levels_.indexOf(level, histTo);
    const double period = [&] {
        const auto it = periodMs_.find(signalId);
        return it == periodMs_.end() ? 0.0 : it->second;
    }();
    GapState gs;
    for (std::int64_t i = i0; i <= i1; ++i) {
        const double tileLo = static_cast<double>(i * span);
        const double tileHi = tileLo + static_cast<double>(span) - 1.0;
        const double lo = std::max(static_cast<double>(fromMs), tileLo);
        const double hi = std::min(static_cast<double>(histTo), tileHi);
        if (hi < lo) {
            continue;
        }
        const Tile* tile = find(signalId, level, i);
        if (tile != nullptr && tile->haveData) {
            touch(*tile);
            appendTile(out, *tile, level, lo, hi, period, level > 0 ? static_cast<double>(levels_.bucketMs(level)) : 0.0,
                       gs);
            continue;
        }
        // Not here yet: a coarser tile that covers this span, or else loading.
        out.incomplete = true;
        gs.have = false;
        bool filled = false;
        for (int coarse = level + 1; coarse <= levels_.top() && !filled; ++coarse) {
            const Tile* c = find(signalId, coarse, levels_.indexOf(coarse, tileLo));
            if (c == nullptr || !c->haveData) {
                continue;
            }
            touch(*c);
            GapState cg;
            appendTile(out, *c, coarse, lo, hi, 0.0, static_cast<double>(levels_.bucketMs(coarse)), cg);
            filled = true;
        }
        if (!filled) {
            pushPoint(out, lo, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::quiet_NaN(), out.envelope);
        }
    }
    foldPlot(out, maxPoints);
    return out;
}

PlotArrays TileCache::gatherL0(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const {
    PlotArrays out;
    if (toMs < fromMs || signalId == 0 || !levels_.valid()) {
        return out;
    }
    const std::int64_t i0 = levels_.indexOf(0, fromMs);
    const std::int64_t i1 = levels_.indexOf(0, toMs);
    for (std::int64_t i = i0; i <= i1; ++i) {
        const Tile* tile = find(signalId, 0, i);
        if (tile == nullptr || !tile->haveData) {
            out.incomplete = true;
            continue;
        }
        touch(*tile);
        for (std::size_t k = 0; k < tile->t.size(); ++k) {
            const double t = tile->t[k];
            if (t < static_cast<double>(fromMs) || t > static_cast<double>(toMs)) {
                continue;
            }
            if (k >= tile->rawV.size()) {
                out.incomplete = true;
                continue;
            }
            out.t.push_back(t);
            out.y.push_back(static_cast<double>(tile->rawV[k]));
        }
    }
    return out;
}

void TileCache::stitchRing(PlotArrays& dest, std::span<const double> t, std::span<const double> v, std::size_t count,
                           std::size_t cap, std::size_t write, std::uint64_t fromMs, std::uint64_t toMs,
                           std::uint32_t signalId) const {
    if (count == 0 || t.empty() || v.empty()) {
        return;
    }
    (void)signalId;
    mergeRingLike(dest, t, v, count, cap, write, static_cast<double>(fromMs), static_cast<double>(toMs));
}

std::uint64_t TileCache::pump() {
    ++pumps_;
    for (auto& [key, d] : disk_) {
        (void)key;
        if (d.pending && d.deadlinePump < pumps_) {
            d.pending = false;
        }
    }
    if (store_ == nullptr) {
        return pumps_;
    }
    std::vector<LoadedTileRange> done;
    store_->takeCompleted(done);
    for (const auto& range : done) {
        const std::int64_t span = std::min<std::int64_t>(range.indexTo - range.indexFrom + 1, kMaxTilesPerWindow);
        for (std::int64_t i = 0; i < span; ++i) {
            if (const auto d = disk_.find(makeKey(range.signalId, range.level, range.indexFrom + i));
                d != disk_.end()) {
                d->second.pending = false;
            }
        }
        for (const auto& blob : range.blobs) {
            PersistedTile p;
            if (!decodeTile(blob, p)) {
                continue;
            }
            if (p.runId != runId_ || p.rev != rev_ || p.sha != sha_ || p.signalId != range.signalId ||
                p.level != range.level || p.tileIndex < range.indexFrom || p.tileIndex > range.indexTo ||
                p.level > levels_.top()) {
                continue;
            }
            // The bucket width is the level's: a blob written with another width is not this tile.
            if (p.bucketMs != (p.level > 0 ? static_cast<std::uint32_t>(levels_.bucketMs(p.level)) : 0u)) {
                continue;
            }
            admitPersisted(p);
        }
    }
    std::size_t written = 0;
    while (!writeQueue_.empty() && written < kWritesPerPump) {
        const TileKey key = writeQueue_.front();
        writeQueue_.erase(writeQueue_.begin());
        const auto it = tiles_.find(key);
        if (it == tiles_.end() || it->second.state != TileState::Sealed) {
            continue;
        }
        const Tile& tile = it->second;
        PersistedTile p;
        p.formatVersion = kTileBlobVersion;
        p.runId = key.runId;
        p.rev = key.rev;
        p.sha = key.sha;
        p.signalId = key.signalId;
        p.level = key.level;
        p.tileIndex = key.tileIndex;
        p.bucketMs = key.level > 0 ? static_cast<std::uint32_t>(levels_.bucketMs(key.level)) : 0u;
        p.t = tile.t;
        p.rawV = tile.rawV;
        p.minV = tile.minV;
        p.maxV = tile.maxV;
        p.meanV = tile.meanV;
        const auto blob = encodeTile(p);
        if (blob.size() > kTileBlobMaxBytes) {
            continue;
        }
        if (store_->put(PersistedTileKey{key.signalId, key.level, key.tileIndex}, blob)) {
            ++persistWritten_;
        }
        ++written;
    }
    // A read that has been answered and whose tile is here no longer needs its entry.
    for (auto it = disk_.begin(); it != disk_.end();) {
        const auto tit = tiles_.find(it->first);
        if (!it->second.pending && tit != tiles_.end() && tit->second.haveData) {
            it = disk_.erase(it);
        } else {
            ++it;
        }
    }
    return pumps_;
}

void TileCache::queuePersist(const TileKey& key) {
    if (store_ == nullptr || writeQueue_.size() >= kWriteQueueCap) {
        return;
    }
    writeQueue_.push_back(key);
}

void TileCache::admitPersisted(const PersistedTile& p) {
    const TileKey key = makeKey(p.signalId, p.level, p.tileIndex);
    if (const auto it = tiles_.find(key); it != tiles_.end() && (it->second.haveData || it->second.state == TileState::Requested)) {
        return;
    }
    Tile tile;
    tile.key = key;
    tile.state = TileState::Sealed;
    tile.haveData = true;
    tile.sealed = true;
    tile.t = p.t;
    tile.rawV = p.rawV;
    tile.minV = p.minV;
    tile.maxV = p.maxV;
    tile.meanV = p.meanV;
    Tile& slot = tiles_[key];
    slot = std::move(tile);
    accountTile(slot);
    touch(slot);
    ++persistRestored_;
    evictIfNeeded();
}

}  // namespace photon::ui
