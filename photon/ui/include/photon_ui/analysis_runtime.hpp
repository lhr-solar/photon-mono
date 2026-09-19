#pragma once

#include "photon/control.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/history_io.hpp"
#include "photon_ui/tile_cache.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ui {

// Analysis-owned history (C3). One TileCache + HistoryIO per bound run.
// Never stored on ClientArena. Never call arena.setSession / selectSource /
// arena.tiles() from this path.

inline constexpr std::uint32_t kAnalysisQueryOrigin = 0x80000000u;
inline constexpr std::uint32_t kAnalysisQueryStride = 0x00100000u;

enum class AnalysisRunRole : std::uint8_t { Primary, Overlay };

struct AnalysisRunBinding {
    std::string sessionId;
    std::string dbcHash;
    AnalysisRunRole role = AnalysisRunRole::Primary;
    std::int64_t timeOffsetMs = 0;
    std::string color;
    std::uint64_t minMs = 0;
    std::uint64_t maxMs = 0;
};

struct AnalysisHistoryRequest {
    std::uint32_t queryId = 0;
    std::string sessionId;
    std::uint64_t fromMs = 0;
    std::uint64_t toMs = 0;
    std::vector<std::uint32_t> signalIds;
    std::uint32_t maxPoints = 0;
};

inline bool isAnalysisQueryId(std::uint32_t queryId) {
    return queryId >= kAnalysisQueryOrigin;
}

// primary = native + timeOffsetMs. Offset is render-time only — never a data merge.
inline std::uint64_t analysisNativeMs(std::uint64_t primaryMs, std::int64_t timeOffsetMs) {
    const auto t = static_cast<std::int64_t>(primaryMs) - timeOffsetMs;
    return t < 0 ? 0 : static_cast<std::uint64_t>(t);
}

inline std::uint64_t analysisPrimaryMs(std::uint64_t nativeMs, std::int64_t timeOffsetMs) {
    const auto t = static_cast<std::int64_t>(nativeMs) + timeOffsetMs;
    return t < 0 ? 0 : static_cast<std::uint64_t>(t);
}

inline void applyRenderOffset(PlotArrays& plot, std::int64_t timeOffsetMs) {
    const double d = static_cast<double>(timeOffsetMs);
    for (double& t : plot.t) {
        t += d;
    }
}

// Local HistoryIO. Records query/cancel. Optional forward sends on the socket
// without using Client as the TileCache I/O target (results demux separately).
class AnalysisHistoryIO final : public HistoryIO {
public:
    void requestHistory(std::uint32_t queryId, std::string sessionId, std::uint64_t fromMs,
                        std::uint64_t toMs, std::vector<std::uint32_t> signalIds,
                        std::uint32_t maxPoints) override;
    void cancelHistory(std::uint32_t queryId) override;

    void setForward(HistoryIO* forward) { forward_ = forward; }
    HistoryIO* forward() const { return forward_; }

    const std::vector<AnalysisHistoryRequest>& requests() const { return requests_; }
    const std::vector<std::uint32_t>& cancels() const { return cancels_; }

private:
    HistoryIO* forward_ = nullptr;
    std::vector<AnalysisHistoryRequest> requests_;
    std::vector<std::uint32_t> cancels_;
};

class AnalysisBoundRun {
public:
    explicit AnalysisBoundRun(AnalysisRunBinding binding, std::uint32_t queryOrigin);
    AnalysisBoundRun(const AnalysisBoundRun&) = delete;
    AnalysisBoundRun& operator=(const AnalysisBoundRun&) = delete;
    AnalysisBoundRun(AnalysisBoundRun&&) = delete;
    AnalysisBoundRun& operator=(AnalysisBoundRun&&) = delete;

    void rebind(AnalysisRunBinding binding);
    void setTimeOffsetMs(std::int64_t ms);
    void setColor(std::string color);

    const AnalysisRunBinding& binding() const { return binding_; }
    std::uint32_t queryOrigin() const { return queryOrigin_; }
    TileCache& cache() { return cache_; }
    const TileCache& cache() const { return cache_; }
    AnalysisHistoryIO& io() { return io_; }
    const AnalysisHistoryIO& io() const { return io_; }

    void requestWindow(std::span<const std::uint32_t> signalIds, std::uint64_t fromMs, std::uint64_t toMs,
                       std::uint32_t maxPoints, bool dragging);
    void requestCursorWindow(std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs);
    void cancelLeaving(std::uint64_t fromMs, std::uint64_t toMs);
    PlotArrays gatherPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                          std::uint32_t maxPoints = 0) const;
    PlotArrays gatherL0(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const;
    bool sampleAt(std::uint32_t signalId, std::uint64_t tMs, double& value) const;

    bool ingestResult(const ctrl::Message& msg);
    bool ingestChunk(const proto::HistoryChunk& chunk);

private:
    AnalysisRunBinding binding_;
    std::uint32_t queryOrigin_ = 0;
    TileCache cache_;
    AnalysisHistoryIO io_;

    void applySession();
};

class AnalysisRuntime {
public:
    AnalysisBoundRun* bind(AnalysisRunBinding binding);
    void unbind(std::string_view sessionId);
    void clear();

    AnalysisBoundRun* find(std::string_view sessionId);
    const AnalysisBoundRun* find(std::string_view sessionId) const;
    AnalysisBoundRun* primary();
    const AnalysisBoundRun* primary() const;
    AnalysisBoundRun* overlay();
    const AnalysisBoundRun* overlay() const;

    void setOverlayOffsetMs(std::int64_t ms);
    std::int64_t overlayOffsetMs() const;

    bool hasBoundRun() const { return !runs_.empty(); }
    std::size_t boundCount() const { return runs_.size(); }
    std::size_t residentBytes() const;
    std::size_t usedBytes() const { return residentBytes(); }
    std::size_t overshootBytes() const;
    std::size_t budget() const { return budget_; }
    void setBudget(std::size_t bytes);

    // Socket send only. Never ClientArena::tiles() / setSession / selectSource.
    void setTransport(HistoryIO* io);
    HistoryIO* transport() const { return transport_; }

    void requestWindow(std::span<const std::uint32_t> signalIds, std::uint64_t fromMs, std::uint64_t toMs,
                       std::uint32_t maxPoints, bool dragging);
    void requestCursorWindow(std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs);
    void cancelLeaving(std::uint64_t fromMs, std::uint64_t toMs);
    // Cancel tiles that left the visible window, then request unless scrubbing.
    void tickHistory(std::span<const std::uint32_t> signalIds, std::uint32_t maxPoints);
    PlotArrays gatherPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                          std::uint32_t maxPoints = 0) const;
    PlotArrays gatherOverlayPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                                 std::uint32_t maxPoints = 0) const;
    PlotArrays gatherL0(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const;
    bool sampleAt(std::uint32_t signalId, std::uint64_t tMs, double& value) const;
    void seekCursor(std::uint64_t tMs);

    bool ingestResult(const ctrl::Message& msg);
    bool ingestChunk(const proto::HistoryChunk& chunk);

    // Shared analysis cursor + window. Scrub is client-side; commit on release.
    void setCursorMs(std::uint64_t cursorMs);
    void setWindowMs(std::uint64_t windowMs);
    void beginScrub();
    void previewScrub(std::uint64_t cursorMs);
    void commitScrub();
    void cancelScrub();
    bool scrubbing() const { return scrubbing_; }
    std::uint64_t cursorMs() const { return cursorMs_; }
    std::uint64_t displayCursorMs() const { return scrubbing_ ? previewMs_ : cursorMs_; }
    std::uint64_t windowMs() const { return windowMs_; }
    std::uint64_t windowFromMs() const;
    std::uint64_t windowToMs() const;
    void holdView(std::uint64_t fromMs, std::uint64_t toMs);
    void releaseView();
    bool viewHeld() const { return viewHeld_; }

private:
    std::vector<std::unique_ptr<AnalysisBoundRun>> runs_;
    HistoryIO* transport_ = nullptr;
    std::size_t budget_ = TileCache::kDefaultBudget;
    std::uint32_t nextQueryOrigin_ = kAnalysisQueryOrigin;
    std::uint64_t cursorMs_ = 0;
    std::uint64_t previewMs_ = 0;
    std::uint64_t windowMs_ = 15000;
    bool scrubbing_ = false;
    bool viewHeld_ = false;
    std::uint64_t viewFromMs_ = 0;
    std::uint64_t viewToMs_ = 0;
    bool haveHistWindow_ = false;
    std::uint64_t histFromMs_ = 0;
    std::uint64_t histToMs_ = 0;

    const AnalysisBoundRun* findConst(std::string_view sessionId) const;
    void syncBudgets();
};

}  // namespace photon::ui
