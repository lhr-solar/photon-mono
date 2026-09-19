#pragma once

#include "imgui.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace photon::ui {

struct UiContext;

class LogDrawer {
public:
    void draw(UiContext& ui, ImVec2 size);
    void drawExportPopup(UiContext& ui);
    float occupiedWidth() const { return tabW_ + width_; }
    bool exportDialogOpen() const { return exportOpen_; }

private:
    enum class ExportScope { Mapped, Pick, All };

    bool open_ = false;
    float width_ = 0.0f;
    float tabW_ = 28.0f;
    int calYear_ = 2026;
    int calMonth_ = 7;
    std::string selectedDate_;
    bool calReady_ = false;
    bool catalogDateApplied_ = false;
    int tlLevel_ = 0;
    std::uint8_t tlDragging_ = 0;
    std::uint64_t localCursorMs_ = 0;
    bool draggingTl_ = false;
    double lastTlSendS_ = 0.0;
    bool exportOpen_ = false;
    std::string exportSession_;
    std::uint64_t exportDurMs_ = 0;
    std::uint64_t exportSamples_ = 0;
    int exportLevel_ = 2;
    ExportScope exportScope_ = ExportScope::Mapped;
    char exportFilter_[64]{};
    std::set<std::string> exportPicked_;

    void ensureCalendar(const UiContext& ui);
    void sendScrub(UiContext& ui, std::uint64_t cursorMs, bool force);
    void drawTab(UiContext& ui, ImVec2 size);
    void drawPanel(UiContext& ui, ImVec2 size);
    void drawTimeline(UiContext& ui);
    void drawCalendar(UiContext& ui);
    void drawRuns(UiContext& ui);
    void startExportDownload(const std::vector<std::string>& keys) const;
};

}  // namespace photon::ui
