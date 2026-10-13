#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace photon::ui {

enum class ViewCellKind { Line, Value, Scatter, Histogram };

inline bool sharesTimeAxis(ViewCellKind kind) {
    return kind == ViewCellKind::Line || kind == ViewCellKind::Histogram;
}

struct ViewSignal {
    std::uint32_t id = 0;
    std::uint32_t messageId = 0;
    std::string messageName;
    std::string signalName;
    std::string label;
    // Additive v1 series style. Empty / 0 / false / yAxis=1 are omitted on save.
    std::string color;
    float width = 0.0f;
    bool hidden = false;
    int yAxis = 1;
};

struct ViewRect {
    int x = 0;
    int y = 0;
    int w = 1;
    int h = 1;
};

struct ViewCell {
    std::string id;
    ViewCellKind kind = ViewCellKind::Line;
    std::string title;
    ViewRect rect{};
    float timeWindowSec = 15.0f;
    bool legend = true;
    bool freeze = false;
    // Set by on-plot scrub so Play/Live can release the held window without
    // clearing a Freeze the user checked in the picker.
    bool scrubHold = false;
    double viewMinMs = 0.0;
    double viewMaxMs = 0.0;
    // false = Auto Y (default). Fits the visible window; axis handles are locked.
    bool yLocked = false;
    // Alt-wheel zoom. Kept apart from yLocked, which is the picker's manual lock.
    bool yUser = false;
    double yUserLo = 0.0;
    double yUserHi = 0.0;
    double yUserRightLo = 0.0;
    double yUserRightHi = 0.0;
    double yFitLo = 0.0;
    double yFitHi = 0.0;
    double yFitRightLo = 0.0;
    double yFitRightHi = 0.0;
    bool rangeActive = false;
    bool rangeDragging = false;
    double rangeA = 0.0;
    double rangeB = 0.0;
    std::vector<ViewSignal> sources;
    bool dualY = false;
    // Shared analysis metadata. Empty is omitted on save.
    std::string derivedChannelId;
};

inline constexpr int kGridCols = 24;
inline constexpr float kGridRowH = 24.0f;
inline constexpr int kDefaultCellW = 12;
inline constexpr int kDefaultCellH = 14;

struct CustomView {
    std::string id = "view-1";
    std::string name = "View 1";
    int columns = kGridCols;
    float rowHeight = kGridRowH;
    float gap = 10.0f;
    bool locked = true;
    std::vector<ViewCell> cells;
};

struct CustomViewDocument {
    int schemaVersion = 2;
    std::string id = "custom-views";
    std::string name = "Custom Views";
    int active = 0;
    std::vector<CustomView> views;
};

}  // namespace photon::ui
