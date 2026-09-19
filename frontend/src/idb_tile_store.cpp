#include "photon_ui/tile_store.hpp"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

#include <cstdint>
#include <string>
#include <vector>

namespace photon::ui {
namespace {

// IndexedDB budget. This is a *separate, larger* budget from the 256 MiB
// in-memory cap and does not relax it: a restored tile is admitted through the
// same reservation path as a network tile, so resident bytes cannot grow past
// the in-memory cap no matter how much is on disk.
//
// 512 MiB is chosen as roughly 2x the in-memory cap, so a full working set plus
// the history around it fits, while staying far inside the origin quota browsers
// grant (Chrome offers a large fraction of free disk). It has NOT been measured
// against a real quota; see the report.
constexpr double kIdbBudgetBytes = 512.0 * 1024.0 * 1024.0;

// Object store shape:
//   tiles: keyPath ["scope", "signalId", "level", "tileIndex"]
//          value { scope, signalId, level, tileIndex, bytes: Uint8Array, lastUsed }
//          index "byLastUsed" on "lastUsed" -> LRU pruning
//   meta:  key "totals" -> { bytes }
//
// Range reads use IDBKeyRange.bound over the compound key, so one cursor scan
// serves a whole tile-index span for a (signal, level).
EM_JS(void, photon_tiles_open, (double budgetBytes), {
    if (Module._photonTiles) {
        return;
    }
    const s = {
        db: null,
        failed: false,
        scope: "",
        budget: budgetBytes,
        bytes: 0,
        inFlight: 0,
        done: [],
    };
    Module._photonTiles = s;
    if (typeof indexedDB === 'undefined') {
        s.failed = true;
        return;
    }
    let req;
    try {
        req = indexedDB.open('photon-tiles', 1);
    } catch (e) {
        s.failed = true;
        return;
    }
    req.onupgradeneeded = function(ev) {
        const db = ev.target.result;
        if (!db.objectStoreNames.contains('tiles')) {
            const os = db.createObjectStore('tiles', { keyPath: ['scope', 'signalId', 'level', 'tileIndex'] });
            os.createIndex('byLastUsed', 'lastUsed');
        }
        if (!db.objectStoreNames.contains('meta')) {
            db.createObjectStore('meta');
        }
    };
    req.onsuccess = function(ev) {
        s.db = ev.target.result;
        try {
            const tx = s.db.transaction('meta', 'readonly');
            const g = tx.objectStore('meta').get('totals');
            g.onsuccess = function() {
                if (g.result && typeof g.result.bytes === 'number') {
                    s.bytes = g.result.bytes;
                }
            };
        } catch (e) {
        }
    };
    req.onerror = function() { s.failed = true; };
    req.onblocked = function() { s.failed = true; };
});

EM_JS(int, photon_tiles_ready, (), {
    const s = Module._photonTiles;
    return s && s.db && !s.failed ? 1 : 0;
});

EM_JS(double, photon_tiles_bytes, (), {
    const s = Module._photonTiles;
    return s ? s.bytes : 0;
});

// Drops every record outside the new scope. A session or DBC change is a full
// invalidation, so the old records are unreachable and must not keep occupying
// the budget.
EM_JS(void, photon_tiles_set_scope, (const char* scope), {
    const s = Module._photonTiles;
    if (!s) {
        return;
    }
    s.scope = UTF8ToString(scope);
    if (!s.db || s.failed) {
        return;
    }
    try {
        const tx = s.db.transaction(['tiles', 'meta'], 'readwrite');
        const os = tx.objectStore('tiles');
        const cur = os.openCursor();
        let freed = 0;
        cur.onsuccess = function(ev) {
            const c = ev.target.result;
            if (!c) {
                if (freed > 0) {
                    s.bytes = Math.max(0, s.bytes - freed);
                    tx.objectStore('meta').put({ bytes: s.bytes }, 'totals');
                }
                return;
            }
            if (c.value && c.value.scope !== s.scope) {
                freed += (c.value.bytes ? c.value.bytes.byteLength : 0);
                c.delete();
            }
            c.continue();
        };
    } catch (e) {
    }
});

EM_JS(int, photon_tiles_request, (int signalId, int level, double indexFrom, double indexTo), {
    const s = Module._photonTiles;
    if (!s || !s.db || s.failed || !s.scope || s.inFlight >= 8) {
        return 0;
    }
    const scope = s.scope;
    s.inFlight += 1;
    // Every accepted read pushes exactly one completion, including on error, so
    // the C++ side's deferral of the socket request always gets released.
    const finish = function(blobs) {
        s.inFlight -= 1;
        const sizes = [];
        let total = 0;
        for (const b of blobs) {
            sizes.push(b.byteLength);
            total += b.byteLength;
        }
        const head = 32 + 4 * sizes.length;
        const env = new Uint8Array(head + total);
        const dv = new DataView(env.buffer);
        dv.setUint32(0, 0x31455450, true);
        dv.setUint32(4, signalId >>> 0, true);
        dv.setUint32(8, level >>> 0, true);
        dv.setFloat64(12, indexFrom, true);
        dv.setFloat64(20, indexTo, true);
        dv.setUint32(28, sizes.length >>> 0, true);
        for (let i = 0; i < sizes.length; ++i) {
            dv.setUint32(32 + 4 * i, sizes[i] >>> 0, true);
        }
        let off = head;
        for (const b of blobs) {
            env.set(b, off);
            off += b.byteLength;
        }
        s.done.push(env);
    };
    try {
        const tx = s.db.transaction('tiles', 'readonly');
        const os = tx.objectStore('tiles');
        const range = IDBKeyRange.bound([scope, signalId, level, indexFrom],
                                        [scope, signalId, level, indexTo]);
        const blobs = [];
        const touched = [];
        const cur = os.openCursor(range);
        cur.onsuccess = function(ev) {
            const c = ev.target.result;
            if (!c) {
                finish(blobs);
                if (touched.length > 0) {
                    // Recency updates on read, not only on insert -- otherwise
                    // the prune below is insertion-ordered rather than an LRU.
                    try {
                        const wtx = s.db.transaction('tiles', 'readwrite');
                        const wos = wtx.objectStore('tiles');
                        const now = Date.now();
                        for (const k of touched) {
                            const g = wos.get(k);
                            g.onsuccess = function() {
                                if (g.result) {
                                    g.result.lastUsed = now;
                                    wos.put(g.result);
                                }
                            };
                        }
                    } catch (e) {
                    }
                }
                return;
            }
            if (c.value && c.value.bytes) {
                blobs.push(c.value.bytes);
                touched.push([c.value.scope, c.value.signalId, c.value.level, c.value.tileIndex]);
            }
            c.continue();
        };
        cur.onerror = function() { finish([]); };
        tx.onerror = function() { };
    } catch (e) {
        finish([]);
    }
    return 1;
});

EM_JS(int, photon_tiles_head_size, (), {
    const s = Module._photonTiles;
    return s && s.done.length > 0 ? s.done[0].length : 0;
});

EM_JS(void, photon_tiles_head_copy, (int ptr), {
    const s = Module._photonTiles;
    if (!s || s.done.length === 0) {
        return;
    }
    HEAPU8.set(s.done[0], ptr);
});

EM_JS(void, photon_tiles_pop, (), {
    const s = Module._photonTiles;
    if (s && s.done.length > 0) {
        s.done.shift();
    }
});

EM_JS(int, photon_tiles_put, (int signalId, int level, double tileIndex, int ptr, int len), {
    const s = Module._photonTiles;
    if (!s || !s.db || s.failed || !s.scope) {
        return 0;
    }
    const bytes = HEAPU8.slice(ptr, ptr + len);
    try {
        const tx = s.db.transaction(['tiles', 'meta'], 'readwrite');
        const os = tx.objectStore('tiles');
        const rec = {
            scope: s.scope,
            signalId: signalId >>> 0,
            level: level >>> 0,
            tileIndex: tileIndex,
            bytes: bytes,
            lastUsed: Date.now(),
        };
        const existing = os.get([rec.scope, rec.signalId, rec.level, rec.tileIndex]);
        existing.onsuccess = function() {
            const prev = existing.result && existing.result.bytes ? existing.result.bytes.byteLength : 0;
            s.bytes = Math.max(0, s.bytes - prev) + len;
            os.put(rec);
            if (s.bytes > s.budget) {
                // Prune oldest-first to 85% of budget in the same transaction
                // that pushed it over, so the total can never run away.
                const target = s.budget * 0.85;
                const idx = os.index('byLastUsed');
                const pc = idx.openCursor();
                pc.onsuccess = function(ev) {
                    const c = ev.target.result;
                    if (!c || s.bytes <= target) {
                        tx.objectStore('meta').put({ bytes: s.bytes }, 'totals');
                        return;
                    }
                    const n = c.value && c.value.bytes ? c.value.bytes.byteLength : 0;
                    if (c.value && c.value.scope === rec.scope && c.value.signalId === rec.signalId &&
                        c.value.level === rec.level && c.value.tileIndex === rec.tileIndex) {
                        c.continue();
                        return;
                    }
                    s.bytes = Math.max(0, s.bytes - n);
                    c.delete();
                    c.continue();
                };
            } else {
                tx.objectStore('meta').put({ bytes: s.bytes }, 'totals');
            }
        };
    } catch (e) {
        return 0;
    }
    return 1;
});

class IdbTileStore final : public TileStore {
public:
    void setScope(const std::string& sessionId, const std::string& dbcHash) override {
        ensureOpen();
        // The scope prefix carries the blob format version as well as the session
        // and DBC identity, so a format bump orphans old records and the scope
        // sweep in photon_tiles_set_scope reclaims them.
        scope_ = "v" + std::to_string(kTileBlobVersion) + "|" + sessionId + "|" + dbcHash;
        photon_tiles_set_scope(scope_.c_str());
    }

    bool requestRange(std::uint32_t signalId, std::uint8_t level, std::int64_t indexFrom,
                      std::int64_t indexTo) override {
        if (scope_.empty() || photon_tiles_ready() == 0) {
            return false;
        }
        return photon_tiles_request(static_cast<int>(signalId), static_cast<int>(level),
                                    static_cast<double>(indexFrom), static_cast<double>(indexTo)) != 0;
    }

    std::size_t takeCompleted(std::vector<LoadedTileRange>& out) override {
        std::size_t taken = 0;
        for (int guard = 0; guard < 32; ++guard) {
            const int size = photon_tiles_head_size();
            if (size <= 0) {
                break;
            }
            std::vector<std::uint8_t> env(static_cast<std::size_t>(size));
            photon_tiles_head_copy(reinterpret_cast<int>(env.data()));
            photon_tiles_pop();
            LoadedTileRange range;
            if (decodeLoadedRange(env, range)) {
                out.push_back(std::move(range));
                ++taken;
            }
        }
        storedBytes_ = static_cast<std::size_t>(photon_tiles_bytes());
        return taken;
    }

    bool put(const PersistedTileKey& key, const std::vector<std::uint8_t>& blob) override {
        if (scope_.empty() || blob.empty() || photon_tiles_ready() == 0) {
            return false;
        }
        return photon_tiles_put(static_cast<int>(key.signalId), static_cast<int>(key.level),
                                static_cast<double>(key.tileIndex), reinterpret_cast<int>(blob.data()),
                                static_cast<int>(blob.size())) != 0;
    }

    std::size_t storedBytes() const override { return storedBytes_; }
    std::size_t byteBudget() const override { return static_cast<std::size_t>(kIdbBudgetBytes); }

private:
    // Opening is deferred out of the constructor: this object is a static, and
    // the EM_JS glue touches Module/HEAPU8, which are not dependable during
    // static init.
    void ensureOpen() {
        if (opened_) {
            return;
        }
        opened_ = true;
        photon_tiles_open(kIdbBudgetBytes);
    }

    std::string scope_;
    std::size_t storedBytes_ = 0;
    bool opened_ = false;
};

IdbTileStore gStore;

struct Registrar {
    Registrar() { registerDefaultTileStore(&gStore); }
};

const Registrar gRegistrar;

}  // namespace
}  // namespace photon::ui

#endif  // __EMSCRIPTEN__
