#include "photon_ui/tile_store.hpp"

#include <cstring>

namespace photon::ui {
namespace {

TileStore* gDefaultStore = nullptr;

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

void putU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    putU32(out, static_cast<std::uint32_t>(v & 0xffffffffu));
    putU32(out, static_cast<std::uint32_t>(v >> 32));
}

bool takeU32(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint32_t& v) {
    if (off + 4 > in.size()) {
        return false;
    }
    v = static_cast<std::uint32_t>(in[off]) | (static_cast<std::uint32_t>(in[off + 1]) << 8) |
        (static_cast<std::uint32_t>(in[off + 2]) << 16) | (static_cast<std::uint32_t>(in[off + 3]) << 24);
    off += 4;
    return true;
}

bool takeU64(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint64_t& v) {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    if (!takeU32(in, off, lo) || !takeU32(in, off, hi)) {
        return false;
    }
    v = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
    return true;
}

void putF64Array(std::vector<std::uint8_t>& out, const std::vector<double>& v) {
    for (const double d : v) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        putU64(out, bits);
    }
}

void putF32Array(std::vector<std::uint8_t>& out, const std::vector<float>& v) {
    for (const float f : v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        putU32(out, bits);
    }
}

bool takeF64Array(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint32_t n, std::vector<double>& out) {
    if (off > in.size() || static_cast<std::uint64_t>(n) * 8u > in.size() - off) {
        return false;
    }
    out.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint64_t bits = 0;
        if (!takeU64(in, off, bits)) {
            return false;
        }
        double d = 0;
        std::memcpy(&d, &bits, sizeof(d));
        out[i] = d;
    }
    return true;
}

bool takeF32Array(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint32_t n, std::vector<float>& out) {
    if (off > in.size() || static_cast<std::uint64_t>(n) * 4u > in.size() - off) {
        return false;
    }
    out.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t bits = 0;
        if (!takeU32(in, off, bits)) {
            return false;
        }
        float f = 0;
        std::memcpy(&f, &bits, sizeof(f));
        out[i] = f;
    }
    return true;
}

// Guards the string lengths so a corrupt header cannot make decodeTile try to
// reserve gigabytes before the truncation check fires.
constexpr std::uint32_t kMaxIdLen = 256;

}  // namespace

std::vector<std::uint8_t> encodeTile(const PersistedTile& tile) {
    std::vector<std::uint8_t> out;
    const auto sampleCount = static_cast<std::uint32_t>(tile.t.size());
    out.reserve(64 + tile.t.size() * 8 +
                (tile.minV.size() + tile.maxV.size() + tile.meanV.size() + tile.rawV.size()) * 4);
    putU32(out, kTileBlobMagic);
    putU32(out, kTileBlobVersion);
    putU32(out, tile.signalId);
    putU32(out, tile.level);
    putU64(out, static_cast<std::uint64_t>(tile.tileIndex));
    putU32(out, tile.bucketMs);
    putU32(out, sampleCount);
    putU32(out, static_cast<std::uint32_t>(tile.minV.size()));
    putU32(out, static_cast<std::uint32_t>(tile.maxV.size()));
    putU32(out, static_cast<std::uint32_t>(tile.meanV.size()));
    putU32(out, static_cast<std::uint32_t>(tile.rawV.size()));
    putU32(out, static_cast<std::uint32_t>(tile.sessionId.size()));
    putU32(out, static_cast<std::uint32_t>(tile.dbcHash.size()));
    out.insert(out.end(), tile.sessionId.begin(), tile.sessionId.end());
    out.insert(out.end(), tile.dbcHash.begin(), tile.dbcHash.end());
    putF64Array(out, tile.t);
    putF32Array(out, tile.minV);
    putF32Array(out, tile.maxV);
    putF32Array(out, tile.meanV);
    putF32Array(out, tile.rawV);
    return out;
}

bool decodeTile(const std::vector<std::uint8_t>& blob, PersistedTile& out) {
    if (blob.size() > kTileBlobMaxBytes) {
        return false;
    }
    std::size_t off = 0;
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!takeU32(blob, off, magic) || magic != kTileBlobMagic) {
        return false;
    }
    if (!takeU32(blob, off, version) || version != kTileBlobVersion) {
        return false;
    }
    std::uint32_t level = 0;
    std::uint64_t tileIndex = 0;
    std::uint32_t sampleCount = 0;
    std::uint32_t minCount = 0;
    std::uint32_t maxCount = 0;
    std::uint32_t meanCount = 0;
    std::uint32_t rawCount = 0;
    std::uint32_t sessionLen = 0;
    std::uint32_t dbcLen = 0;
    PersistedTile t;
    t.formatVersion = version;
    if (!takeU32(blob, off, t.signalId) || !takeU32(blob, off, level) || !takeU64(blob, off, tileIndex) ||
        !takeU32(blob, off, t.bucketMs) || !takeU32(blob, off, sampleCount) || !takeU32(blob, off, minCount) ||
        !takeU32(blob, off, maxCount) || !takeU32(blob, off, meanCount) || !takeU32(blob, off, rawCount) ||
        !takeU32(blob, off, sessionLen) || !takeU32(blob, off, dbcLen)) {
        return false;
    }
    if (level > 5 || sessionLen > kMaxIdLen || dbcLen > kMaxIdLen) {
        return false;
    }
    t.level = static_cast<std::uint8_t>(level);
    t.tileIndex = static_cast<std::int64_t>(tileIndex);
    if (off + sessionLen + dbcLen > blob.size()) {
        return false;
    }
    t.sessionId.assign(blob.begin() + static_cast<std::ptrdiff_t>(off),
                       blob.begin() + static_cast<std::ptrdiff_t>(off + sessionLen));
    off += sessionLen;
    t.dbcHash.assign(blob.begin() + static_cast<std::ptrdiff_t>(off),
                     blob.begin() + static_cast<std::ptrdiff_t>(off + dbcLen));
    off += dbcLen;
    if (!takeF64Array(blob, off, sampleCount, t.t) || !takeF32Array(blob, off, minCount, t.minV) ||
        !takeF32Array(blob, off, maxCount, t.maxV) || !takeF32Array(blob, off, meanCount, t.meanV) ||
        !takeF32Array(blob, off, rawCount, t.rawV)) {
        return false;
    }
    if (t.t.size() != sampleCount || off != blob.size()) {
        return false;
    }
    out = std::move(t);
    return true;
}

std::vector<std::uint8_t> encodeLoadedRange(const LoadedTileRange& range) {
    std::vector<std::uint8_t> out;
    std::size_t payload = 0;
    for (const auto& b : range.blobs) {
        payload += b.size();
    }
    out.reserve(24 + range.blobs.size() * 4 + payload);
    putU32(out, kRangeEnvelopeMagic);
    putU32(out, range.signalId);
    putU32(out, range.level);
    putF64Array(out, {static_cast<double>(range.indexFrom), static_cast<double>(range.indexTo)});
    putU32(out, static_cast<std::uint32_t>(range.blobs.size()));
    for (const auto& b : range.blobs) {
        putU32(out, static_cast<std::uint32_t>(b.size()));
    }
    for (const auto& b : range.blobs) {
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

bool decodeLoadedRange(const std::vector<std::uint8_t>& blob, LoadedTileRange& out) {
    std::size_t off = 0;
    std::uint32_t magic = 0;
    std::uint32_t level = 0;
    std::uint32_t blobCount = 0;
    LoadedTileRange r;
    if (!takeU32(blob, off, magic) || magic != kRangeEnvelopeMagic) {
        return false;
    }
    if (!takeU32(blob, off, r.signalId) || !takeU32(blob, off, level)) {
        return false;
    }
    if (level > 5) {
        return false;
    }
    r.level = static_cast<std::uint8_t>(level);
    std::vector<double> idx;
    if (!takeF64Array(blob, off, 2, idx)) {
        return false;
    }
    r.indexFrom = static_cast<std::int64_t>(idx[0]);
    r.indexTo = static_cast<std::int64_t>(idx[1]);
    if (!takeU32(blob, off, blobCount)) {
        return false;
    }
    if (static_cast<std::uint64_t>(blobCount) * 4u > blob.size() - off) {
        return false;
    }
    std::vector<std::uint32_t> sizes(blobCount);
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < blobCount; ++i) {
        if (!takeU32(blob, off, sizes[i])) {
            return false;
        }
        total += sizes[i];
    }
    if (total != blob.size() - off) {
        return false;
    }
    r.blobs.resize(blobCount);
    for (std::uint32_t i = 0; i < blobCount; ++i) {
        r.blobs[i].assign(blob.begin() + static_cast<std::ptrdiff_t>(off),
                          blob.begin() + static_cast<std::ptrdiff_t>(off + sizes[i]));
        off += sizes[i];
    }
    out = std::move(r);
    return true;
}

TileStore* defaultTileStore() {
    return gDefaultStore;
}

void registerDefaultTileStore(TileStore* store) {
    gDefaultStore = store;
}

}  // namespace photon::ui
