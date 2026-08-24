#pragma once

#include "imgui.h"
#include "photon/control.hpp"
#include "photon_ui/arena.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace photon::ui {

struct UiContext;

enum class ViewCellKind { Line, Value, Scatter };

struct ViewSignal {
    std::uint32_t id = 0;
    std::uint32_t messageId = 0;
    std::string messageName;
    std::string signalName;
    std::string label;
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
    std::vector<ViewSignal> sources;
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
    std::vector<ViewCell> cells;
};

struct CustomViewDocument {
    int schemaVersion = 1;
    std::string id = "custom-views";
    std::string name = "Custom Views";
    int active = 0;
    std::vector<CustomView> views;
};

class CustomViews {
public:
    void ensureLoaded();
    void draw(UiContext& ui);

private:
    CustomViewDocument doc_{};
    bool loaded_ = false;
    bool dirty_ = false;
    int selectedCell_ = -1;
    int nextView_ = 2;
    int nextCell_ = 2;
    char search_[128]{};
    char renameBuf_[64]{};
    bool renameOpen_ = false;
    std::string status_;
    enum class DragKind { None, Move, Resize };
    DragKind drag_ = DragKind::None;
    int dragIndex_ = -1;
    ViewRect dragStart_{};
    ImVec2 dragMouse0_{};
    bool dragDirty_ = false;

    CustomView& active();
    const CustomView& active() const;
    void ensureDefault();
    void markDirty();
    void save() const;
    void exportFile() const;
    void requestImport();
    void pollImport();
    bool applyJson(std::string_view text, std::string* error);
    void addView();
    void deleteActiveView();
    void addCell(ViewCellKind kind);
    void addGgCell();
    void migrateView(CustomView& view);
    ViewCell* targetCell();
    void assignSignal(const ctrl::CatalogSignal& sig);
    void resolve(const ClientArena& arena);
    void subscribe(UiContext& ui);
    void drawToolbar();
    void drawPicker(UiContext& ui, float width);
    void drawGrid(UiContext& ui, ImVec2 size);
    void drawCell(UiContext& ui, ViewCell& cell, ImVec2 size, int index);
    void updateDrag(ImVec2 origin, float unitW, float unitH);
};

std::string serializeCustomViews(const CustomViewDocument& doc);
bool parseCustomViews(std::string_view text, CustomViewDocument* out, std::string* error);

}  // namespace photon::ui
