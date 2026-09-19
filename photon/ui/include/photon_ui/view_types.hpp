#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace photon::ui {

enum class ViewCellKind { Line, Value, Scatter };

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
    bool rangeActive = false;
    bool rangeDragging = false;
    double rangeA = 0.0;
    double rangeB = 0.0;
    std::vector<ViewSignal> sources;
    bool dualY = false;
    // Analysis workbook v2. Empty is omitted on save. Plots v1 ignores this field.
    std::string derivedChannelId;
};

inline constexpr int kGridCols = 12;
inline constexpr float kGridRowH = 160.0f;
inline constexpr int kDefaultCellW = 6;
inline constexpr int kDefaultCellH = 2;

struct CustomView {
    std::string id = "view-1";
    std::string name = "View 1";
    int columns = kGridCols;
    float rowHeight = kGridRowH;
    float gap = 10.0f;
    bool locked = false;
    std::vector<ViewCell> cells;
};

struct CustomViewDocument {
    int schemaVersion = 1;
    std::string id = "custom-views";
    std::string name = "Custom Views";
    int active = 0;
    std::vector<CustomView> views;
};

}  // namespace photon::ui
