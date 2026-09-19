#pragma once

#include "photon/control.hpp"
#include "photon_ui/analysis_runtime.hpp"
#include "photon_ui/analysis_workbook.hpp"
#include "photon_ui/tile_cache.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ui {

struct AnalysisFormulaSpec {
    std::uint32_t leftId = 0;
    std::uint32_t rightId = 0;
    double leftConst = 0.0;
    double rightConst = 0.0;
    bool leftIsConst = false;
    bool rightIsConst = false;
    char op = '+';
    std::string expr;
    std::string leftName;
    std::string rightName;
};

struct AnalysisFormulaResult {
    PlotArrays plot;
    bool decimated = false;
};

inline constexpr const char* kAnalysisDerivedMessage = "derived";
inline constexpr const char* kAnalysisEventThreshold = "threshold";
inline constexpr const char* kAnalysisEventManual = "manual";

bool parseFormulaSpec(std::string_view expr, AnalysisFormulaSpec* out);
void resolveFormulaIds(AnalysisFormulaSpec* spec, const std::vector<ctrl::CatalogSignal>& catalog);
double applyFormulaOp(char op, double a, double b);
AnalysisFormulaResult evaluateFormula(const AnalysisBoundRun& run, const AnalysisFormulaSpec& spec,
                                      std::uint64_t fromMs, std::uint64_t toMs);

std::vector<AnalysisEvent> extractThresholdEvents(const AnalysisBoundRun& run, const AnalysisTransform& xf,
                                                  std::uint64_t fromMs, std::uint64_t toMs);
void replaceThresholdEvents(std::vector<AnalysisEvent>* events, std::vector<AnalysisEvent> extracted);
void collectFormulaSignalIds(const AnalysisFormulaSpec& spec, std::vector<std::uint32_t>* ids);

inline bool analysisDerivedSource(const ViewSignal& src) {
    return src.messageName == kAnalysisDerivedMessage;
}

}  // namespace photon::ui
