#pragma once

#include "photon/control.hpp"
#include "photon_ui/analysis_runtime.hpp"
#include "photon_ui/tile_cache.hpp"
#include "photon_ui/view_types.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ui {

inline constexpr const char* kAnalysisWorkbookStorageKey = "photon-next.analysis-workbooks";
inline constexpr const char* kAnalysisWorkbookSchema = "../docs/config/photon-analysis-workbook.schema.json";
inline constexpr int kAnalysisWorkbookSchemaVersion = 2;

using AnalysisCellKind = ViewCellKind;
using AnalysisCell = ViewCell;
using AnalysisSignal = ViewSignal;
using AnalysisView = CustomView;

struct AnalysisEvent {
    std::string id;
    std::uint64_t tMs = 0;
    std::optional<std::uint64_t> endMs;
    std::string source;
    std::uint32_t signalId = 0;
    std::string label;
    std::optional<double> value;
};

struct AnalysisTransform {
    std::string id;
    std::string name;
    std::uint32_t signalId = 0;
    std::string messageName;
    std::string signalName;
    std::string kind;
    std::string spec;
    std::string label;
    std::optional<double> threshold;
    std::optional<double> min;
    std::optional<double> max;
};

struct AnalysisDerivedChannel {
    std::string id;
    std::string name;
    std::string unit;
    std::string kind;
    std::string spec;
};

struct AnalysisWorkbook {
    int schemaVersion = kAnalysisWorkbookSchemaVersion;
    std::string id = "analysis-workbook";
    std::string name = "Analysis";
    int active = 0;
    std::vector<CustomView> views;
    std::vector<AnalysisRunBinding> runBindings;
    std::vector<AnalysisEvent> events;
    std::vector<AnalysisTransform> transforms;
    std::vector<AnalysisDerivedChannel> derivedChannels;
};

struct AnalysisCursor {
    std::uint64_t committedMs = 0;
    std::uint64_t previewMs = 0;
    bool previewing = false;
    float windowSec = 15.0f;

    std::uint64_t displayMs() const { return previewing ? previewMs : committedMs; }

    std::uint64_t windowFromMs() const {
        const auto end = displayMs();
        const auto span = static_cast<std::uint64_t>(std::max(2.0f, windowSec) * 1000.0f);
        return end > span ? end - span : 0;
    }

    std::uint64_t windowToMs() const { return displayMs(); }

    void beginPreview(std::uint64_t ms) {
        previewing = true;
        previewMs = ms;
    }

    void updatePreview(std::uint64_t ms) { previewMs = ms; }

    void commit() {
        committedMs = previewMs;
        previewing = false;
    }

    void cancelPreview() { previewing = false; }

    void clampTo(std::uint64_t minMs, std::uint64_t maxMs) {
        if (maxMs < minMs) {
            return;
        }
        auto clamp = [&](std::uint64_t t) {
            if (t < minMs) {
                return minMs;
            }
            if (t > maxMs) {
                return maxMs;
            }
            return t;
        };
        committedMs = clamp(committedMs);
        previewMs = clamp(previewMs);
    }
};

std::string serializeAnalysisWorkbook(const AnalysisWorkbook& doc);
bool parseAnalysisWorkbook(std::string_view text, AnalysisWorkbook* out, std::string* error);
inline std::string serializeAnalysisWorkbooks(const AnalysisWorkbook& doc) {
    return serializeAnalysisWorkbook(doc);
}
inline bool parseAnalysisWorkbooks(std::string_view text, AnalysisWorkbook* out, std::string* error) {
    return parseAnalysisWorkbook(text, out, error);
}

void ensureAnalysisWorkbook(AnalysisWorkbook& doc);
CustomView& activeAnalysisView(AnalysisWorkbook& doc);
const CustomView& activeAnalysisView(const AnalysisWorkbook& doc);

bool sameAnalysisSignal(const ViewSignal& src, const ctrl::CatalogSignal& sig);
ViewSignal viewSignalFromCatalog(const ctrl::CatalogSignal& sig);
void resolveAnalysisSignals(AnalysisWorkbook& doc, const std::vector<ctrl::CatalogSignal>& catalog);
std::vector<std::uint32_t> collectWorkbookSignalIds(const AnalysisWorkbook& doc);

void upsertPrimaryBinding(AnalysisWorkbook& doc, const AnalysisRunBinding& bind);
void upsertOverlayBinding(AnalysisWorkbook& doc, const AnalysisRunBinding& bind);
AnalysisRunBinding bindingFromCatalog(const ctrl::Message::CatalogSession& row);
bool catalogMatchesListing(const ctrl::Message::CatalogSession& row, std::string_view id, std::uint64_t minMs,
                           std::uint64_t maxMs, std::string_view dbcHash);

inline constexpr double kPlotScatterPairMinTolMs = 1.0;
inline constexpr double kPlotScatterPairDefaultTolMs = 50.0;
inline constexpr double kPlotScatterPairMaxTolMs = 250.0;

// Pair independently decimated X/Y. maxDtMs <= 0 uses an adaptive tolerance
// from sample spacing (clamped to [default, max]). Exact integer timestamps
// are not required — nearest-neighbor or linear interpolation within the bound.
void pairPlotScatter(const PlotArrays& x, const PlotArrays& y, std::vector<double>& xs, std::vector<double>& ys,
                     double maxDtMs = 0.0);

}  // namespace photon::ui
