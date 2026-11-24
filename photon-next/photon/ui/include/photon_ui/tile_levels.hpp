#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// Tile level math for protocol v2 (photon-next/docs/protocol-v2.md, "Levels and
// tiles" and "Choosing a level"). The bucket widths come from hello_ack.levels;
// nothing here hard-codes a level number. Level 0 holds raw samples and its tile
// covers 10 s. A tile of level L > 0 covers levels[L] * 1024 ms, with 1024 buckets.
// Tile index i covers [i * span, (i + 1) * span) in Unix ms.
namespace photon::ui {

inline constexpr std::int64_t kRawTileSpanMs = 10000;
inline constexpr std::int64_t kTileBuckets = 1024;

class TileLevels {
public:
    TileLevels() = default;
    explicit TileLevels(std::vector<std::int64_t> bucketMs) : bucketMs_(std::move(bucketMs)) {}

    // Usable only when the server gave a list that starts with raw (0).
    bool valid() const { return !bucketMs_.empty() && bucketMs_.front() == 0; }
    int count() const { return static_cast<int>(bucketMs_.size()); }
    // The coarsest level. Overview tiles are this level.
    int top() const { return count() - 1; }
    std::int64_t bucketMs(int level) const { return bucketMs_[static_cast<std::size_t>(level)]; }
    std::int64_t spanMs(int level) const {
        return level <= 0 ? kRawTileSpanMs : bucketMs(level) * kTileBuckets;
    }
    // Tile index of time tMs (Unix ms) at a level. Times before 0 map to tile 0.
    std::int64_t indexOf(int level, double tMs) const {
        if (!valid() || level < 0 || level > top() || tMs <= 0.0) {
            return 0;
        }
        return static_cast<std::int64_t>(tMs / static_cast<double>(spanMs(level)));
    }
    std::int64_t indexOf(int level, std::uint64_t tMs) const {
        return indexOf(level, static_cast<double>(tMs));
    }
    // The coarsest level whose bucket is at most visibleMs / pixels, so there is one
    // bucket per pixel or finer. Level 0 (raw) only when even the first bucketed
    // level is coarser than a pixel, and always when pixels is 0 (no plot to fit).
    int chooseLevel(std::uint64_t visibleMs, std::uint32_t pixels) const {
        if (!valid() || pixels == 0 || top() < 1) {
            return 0;
        }
        const double perPixelMs = static_cast<double>(visibleMs) / static_cast<double>(pixels);
        int best = 0;
        for (int level = 1; level <= top(); ++level) {
            if (static_cast<double>(bucketMs(level)) <= perPixelMs) {
                best = level;
            }
        }
        return best;
    }

private:
    std::vector<std::int64_t> bucketMs_;
};

}  // namespace photon::ui
