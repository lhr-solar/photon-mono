#pragma once

#include "ui/view_types.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace photon::ui {

struct GridEntry {
    std::string id;
    ViewRect rect;
    bool fixed = false;
    int minW = 1;
};

inline bool gridOverlap(const ViewRect& a, const ViewRect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

inline bool sameGridRect(const ViewRect& a, const ViewRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

inline ViewRect boundedGridRect(ViewRect r, int minW = 1, int minH = 1) {
    r.w = std::clamp(r.w, std::min(minW, kGridCols), kGridCols);
    r.h = std::clamp(r.h, minH, 4096);
    r.x = std::clamp(r.x, 0, kGridCols - r.w);
    r.y = std::clamp(r.y, 0, 100000);
    return r;
}

// Resolve from the gesture snapshot, never from the previous preview. This
// makes reversing a drag restore neighboring panels instead of ratcheting them.
inline std::vector<GridEntry> reflowGrid(std::vector<GridEntry> entries, int active, ViewRect candidate) {
    if (active < 0 || active >= static_cast<int>(entries.size()) || entries[active].fixed) return entries;
    const auto before = entries[active].rect;
    const bool horizontal = candidate.y == before.y && (candidate.x != before.x || candidate.w != before.w);
    candidate = boundedGridRect(candidate);
    bool hit = true;
    while (hit) {
        hit = false;
        for (const auto& e : entries) {
            if (e.fixed && gridOverlap(candidate, e.rect)) {
                candidate.y = e.rect.y + e.rect.h;
                hit = true;
            }
        }
    }
    entries[active].rect = candidate;
    std::vector<int> order;
    std::vector<ViewRect> occupied{candidate};
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (i == active) continue;
        if (entries[i].fixed)
            occupied.push_back(entries[i].rect);
        else
            order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& x = entries[a];
        const auto& y = entries[b];
        if (x.rect.y != y.rect.y) return x.rect.y < y.rect.y;
        if (x.rect.x != y.rect.x) return x.rect.x < y.rect.x;
        return x.id < y.id;
    });
    for (int i : order) {
        auto& r = entries[i].rect;
        // Sideways edits share the row by trimming the neighbor on the side
        // being entered. The snapshot restores its width on a reverse drag.
        if (horizontal && r.y == candidate.y && gridOverlap(r, candidate)) {
            const int right = r.x + r.w;
            if (r.x < candidate.x && candidate.x - r.x >= entries[i].minW) {
                r.w = candidate.x - r.x;
            } else if (right > candidate.x + candidate.w &&
                       right - candidate.x - candidate.w >= entries[i].minW) {
                r.x = candidate.x + candidate.w;
                r.w = right - r.x;
            }
        }
        hit = true;
        while (hit) {
            hit = false;
            for (const auto& placed : occupied) {
                if (gridOverlap(r, placed)) {
                    r.y = placed.y + placed.h;
                    hit = true;
                }
            }
        }
        occupied.push_back(r);
    }
    return entries;
}

inline void migrateGridUnits(CustomView& view) {
    const int columns = std::max(1, view.columns);
    const float row = std::max(1.0f, view.rowHeight);
    for (auto& c : view.cells) {
        c.rect.x = static_cast<int>(std::lround(c.rect.x * static_cast<double>(kGridCols) / columns));
        c.rect.w = static_cast<int>(std::lround(c.rect.w * static_cast<double>(kGridCols) / columns));
        c.rect.y = static_cast<int>(std::lround(c.rect.y * row / kGridRowH));
        c.rect.h = static_cast<int>(std::lround(c.rect.h * row / kGridRowH));
        c.rect = boundedGridRect(c.rect, 1, c.kind == ViewCellKind::Value ? 1 : 13);
    }
    view.columns = kGridCols;
    view.rowHeight = kGridRowH;
}

// While exploring a window, never shrink the axis on an incomplete tile.
struct ExploringValueRange {
    bool have = false;
    double lo = 0.0, hi = 1.0;
    void include(double a, double b) {
        if (!std::isfinite(a) || !std::isfinite(b) || !(b > a)) return;
        if (!have) {
            lo = a;
            hi = b;
            have = true;
        } else {
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
    }
};

}  // namespace photon::ui
