#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace photon::ui {

// Persistence seam for sealed history tiles. The only implementation that talks
// to IndexedDB lives in idb_tile_store.cpp and is compiled for WASM only; every
// decision about *what* is stored, *when* it is stored, and how a restored tile
// re-enters the cache is in tile_cache.cpp so it can be tested natively against
// a fake store.
//
// The store never interprets a payload. TileCache hands it an opaque blob and
// gets the same bytes back, so the encode/decode/validate path is ordinary C++
// covered by native tests rather than browser-only glue.

// Identifies a record *within* the current scope. Session and DBC identity are
// deliberately absent: they live in the scope set by setScope, so they cannot
// disagree with it on a per-record basis. The blob itself still carries them, and
// TileCache re-checks them on read.
struct PersistedTileKey {
    std::uint32_t signalId = 0;
    std::uint8_t level = 0;
    std::int64_t tileIndex = 0;
};

// One decoded record. `sampleCount` is not redundant with `t.size()`: it is the
// count the writer recorded, and decodeTile rejects a blob where the two
// disagree, which is what catches a truncated or spliced payload.
struct PersistedTile {
    std::uint32_t formatVersion = 0;
    std::string sessionId;
    std::string dbcHash;
    std::uint32_t signalId = 0;
    std::uint8_t level = 0;
    std::int64_t tileIndex = 0;
    std::uint32_t bucketMs = 0;
    std::vector<double> t;
    std::vector<float> minV;
    std::vector<float> maxV;
    std::vector<float> meanV;
    std::vector<float> rawV;
};

// Result of one range read. `blobs` may be shorter than the requested span --
// absence is normal, and the completion itself is what releases TileCache's
// deferral of the network request for the uncovered indices.
struct LoadedTileRange {
    std::uint32_t signalId = 0;
    std::uint8_t level = 0;
    std::int64_t indexFrom = 0;
    std::int64_t indexTo = 0;
    std::vector<std::vector<std::uint8_t>> blobs;
};

class TileStore {
public:
    virtual ~TileStore() = default;

    // Scope = (sessionId, dbcHash). Reads must select only the current scope;
    // retain other scopes within the shared budget for revisiting logs.
    virtual void setScope(const std::string& sessionId, const std::string& dbcHash) = 0;

    // Non-blocking. Returns false when the store cannot take the read (not open
    // yet, too many in flight, no scope). A `true` return promises exactly one
    // matching entry from takeCompleted() eventually.
    virtual bool requestRange(std::uint32_t signalId, std::uint8_t level, std::int64_t indexFrom,
                              std::int64_t indexTo) = 0;

    // Moves finished reads out of the store. Never blocks.
    virtual std::size_t takeCompleted(std::vector<LoadedTileRange>& out) = 0;

    // Best-effort write. Returns false when dropped.
    virtual bool put(const PersistedTileKey& key, const std::vector<std::uint8_t>& blob) = 0;

    virtual std::size_t storedBytes() const = 0;
    virtual std::size_t byteBudget() const = 0;
};

inline constexpr std::uint32_t kTileBlobMagic = 0x31435450;  // "PTC1"
inline constexpr std::uint32_t kTileBlobVersion = 1;
// A tile larger than this is refused rather than written; it means the level or
// the sample count is not what this cache thinks it is.
inline constexpr std::size_t kTileBlobMaxBytes = 4u * 1024u * 1024u;

std::vector<std::uint8_t> encodeTile(const PersistedTile& tile);

// Envelope the IndexedDB bridge uses to hand one finished range read across the
// JS boundary in a single copy: the JS side only concatenates bytes and writes
// the length table, and all interpretation happens in decodeLoadedRange.
//
//   u32 magic | u32 signalId | u32 level | f64 indexFrom | f64 indexTo
//   u32 blobCount | u32 sizes[blobCount] | blobs...
//
// encodeLoadedRange is the normative description of that layout. It is what the
// native round-trip test drives; the browser path only ever decodes.
inline constexpr std::uint32_t kRangeEnvelopeMagic = 0x31455450;  // "PTE1"

std::vector<std::uint8_t> encodeLoadedRange(const LoadedTileRange& range);
bool decodeLoadedRange(const std::vector<std::uint8_t>& blob, LoadedTileRange& out);

// Returns false on bad magic, unknown version, truncation, a count that does
// not match the payload length, or a size over kTileBlobMaxBytes.
bool decodeTile(const std::vector<std::uint8_t>& blob, PersistedTile& out);

// Registered by idb_tile_store.cpp at static-init time on WASM builds. Native
// builds never register anything, so this returns nullptr and TileCache runs
// exactly as it did before persistence existed.
TileStore* defaultTileStore();
void registerDefaultTileStore(TileStore* store);

}  // namespace photon::ui
