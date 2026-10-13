#pragma once
#include <cstdint>

// Display tile layout is a protocol convention, independent of a disk store.
namespace photon::history {
inline constexpr std::uint32_t kRollupTileBuckets = 1024;
inline constexpr std::uint32_t kL0TileSpanMs = 60000;
inline constexpr std::uint32_t kBuckets[] = {0, 100, 1000, 10000, 60000, 600000};
inline std::uint32_t bucketMsOf(int level) { return level < 0 || level > 5 ? 0 : kBuckets[level]; }
inline std::uint64_t tileSpanMs(int level) { return level <= 0 ? kL0TileSpanMs : static_cast<std::uint64_t>(bucketMsOf(level)) * kRollupTileBuckets; }
inline std::int64_t tileIndexOf(std::uint64_t t, int level) { auto span = tileSpanMs(level); return span == 0 ? 0 : static_cast<std::int64_t>(t / span); }
inline int pickLevel(std::uint64_t from, std::uint64_t to, std::uint32_t points, std::uint64_t count, std::uint8_t built) {
    const auto budget = points == 0 ? 1ull : static_cast<std::uint64_t>(points) * 2;
    int want = 0;
    if (count > budget) {
        const auto span = to > from ? to - from : 0;
        want = 5;
        for (int level = 1; level <= 5; ++level) if (span / bucketMsOf(level) <= budget) { want = level; break; }
    }
    while (want > 0 && !(built & (1u << want))) --want;
    if (!(built & (1u << want))) for (int level = 0; level <= 5; ++level) if (built & (1u << level)) return level;
    return want;
}
}
