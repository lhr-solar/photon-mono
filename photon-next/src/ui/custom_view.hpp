#pragma once

#include "imgui.h"
#include "ui/arena.hpp"
#include "ui/chart_grid.hpp"
#include "ui/view_document.hpp"
#include "ui/view_types.hpp"
#include "wire/control.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace photon::ui {

struct UiContext;

class CustomViews {
public:
    void ensureLoaded();
    void draw(UiContext& ui);
    // Play/Live chrome: drop on-plot scrub holds, keep a Freeze the user checked.
    void releasePlayheadScrub();
    // Catalog pin / run switch: drop Freeze, range, and scrub so the plot
    // follows the new session instead of staying on the previous window.
    void followSessionWindow();

private:
    CustomViewDocument doc_{};
    std::map<std::string, ChartGrid> grids_;
    ChartGrid& grid() { return grids_[active().id]; }
    double panHistoryRequestS_ = -1.0;
    bool requestPanHistory_ = true;
    std::vector<ExploringValueRange> panY1_, panY2_;
    bool loaded_ = false;
    bool dirty_ = false;
    int selectedCell_ = -1;
    int nextView_ = 2;
    int nextCell_ = 2;
    char search_[128]{};
    char renameBuf_[64]{};
    bool renameOpen_ = false;
    bool pickerOpen_ = false;
    std::string status_;
    int pickerDayIndex_ = -1;
    struct PlotExportJob {
        bool active = false;
        bool waitDay = false;
        int settleFrames = 0;
        int cell = -1;
        std::string date;
        std::string path;
        std::string filename;
        double startMs = 0.0;
        double readySinceMs = 0.0;
    };
    PlotExportJob export_{};
    bool plotScrubbing_ = false;
    struct LayoutUndo {
        bool have = false;
        int viewIndex = -1;
        std::vector<ViewCell> cells;
        int selectedCell = -1;
        int nextCell = 0;
    };
    LayoutUndo undo_{};
    // Previous frame's plot hover, so every panel can draw one shared crosshair.
    bool sharedHover_ = false;
    double sharedHoverMs_ = 0.0;
    int sharedHoverGen_ = 0;
    int drawGen_ = 0;
    // Click sets the playhead. A drag slides the window without seeking.
    bool plotPan_ = false;
    int plotPanCell_ = -1;
    bool plotPanMoved_ = false;
    bool plotPanTimeMoved_ = false;
    bool plotPanValueMoved_ = false;
    bool plotPanValueAllowed_ = false;
    double plotPanLo_ = 0.0;
    double plotPanHi_ = 0.0;
    double plotPanGrabT_ = 0.0;
    float plotPanGrabX_ = 0.0f;
    float plotPanGrabY_ = 0.0f;
    float plotPanPlotW_ = 1.0f;
    float plotPanPlotH_ = 1.0f;
    double plotPanYLo_ = 0.0, plotPanYHi_ = 1.0;
    double plotPanYRightLo_ = 0.0, plotPanYRightHi_ = 1.0;
    double lastPlotSeekS_ = -1.0;

    CustomView& active();
    const CustomView& active() const;
    void ensureDefault();
    void pushLayoutUndo();
    void undoLastLayout();
    void markDirty();
    void touchDirty();
    void save() const;
    void flushDirty();
    bool anyScrubHold() const;
    void exportFile() const;
    void requestImport();
    void pollImport();
    bool applyJson(std::string_view text, std::string* error);
    void addView();
    void deleteActiveView();
    void addCell(ViewCellKind kind);
    void addGgCell();
    void addPitVitalsView();
    CustomView makePitVitalsView() const;
    // Returns true when the stock pit board gained a panel.
    bool migrateView(CustomView& view);
    ViewCell* targetCell();
    void assignSignal(const ctrl::CatalogSignal& sig);
    void resolve(const ClientArena& arena);
    void subscribe(UiContext& ui);
    void drawToolbar(UiContext& ui);
    void openPicker();
    void drawPicker(UiContext& ui);
    void drawGrid(UiContext& ui, ImVec2 size);
    void drawCell(UiContext& ui, ViewCell& cell, ImVec2 size, int index);
    void syncPickerDay(const ClientArena& arena);
    bool drawDriveDayCombo(UiContext& ui, const char* id, float width);
    void requestPlotPng(UiContext& ui);
    void tickPlotExport(UiContext& ui);
    bool cellHasSamples(const ClientArena& arena, int index) const;
};

void flushPlotImageExport();

// Telemetry_Status is an empty DBC message and is absent from the recorded logs.
inline const char* antennaCanNote() {
    return "Not on CAN. Telemetry_Status (1792) has no fields in carcan_telemcan.dbc and does not appear in "
           "the logs.";
}

// First catalog signal that looks like a radio antenna reading (name contains
// antenna, rssi, or snr), otherwise the first signal on Telemetry_Status.
const ctrl::CatalogSignal* findAntennaSignal(const ClientArena& arena);

std::string serializeCustomViews(const CustomViewDocument& doc);
bool parseCustomViews(std::string_view text, CustomViewDocument* out, std::string* error);

}  // namespace photon::ui
