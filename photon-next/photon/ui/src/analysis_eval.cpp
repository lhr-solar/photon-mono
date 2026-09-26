#include "photon_ui/analysis_eval.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>

namespace photon::ui {
namespace {

void skipWs(std::string_view& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
}

bool parseTerm(std::string_view& s, std::uint32_t* id, double* constant, bool* isConst, std::string* name) {
    skipWs(s);
    if (s.empty()) {
        return false;
    }
    bool forceId = false;
    if (s.front() == '$') {
        forceId = true;
        s.remove_prefix(1);
    }
    if (!s.empty() && (std::isdigit(static_cast<unsigned char>(s.front())) != 0 || s.front() == '.' ||
                       s.front() == '+' || s.front() == '-')) {
        char* end = nullptr;
        const std::string buf(s);
        const double n = std::strtod(buf.c_str(), &end);
        if (end == buf.c_str()) {
            return false;
        }
        const auto consumed = static_cast<std::size_t>(end - buf.c_str());
        const bool hadDot = buf.find('.') < consumed;
        s.remove_prefix(consumed);
        if (forceId || (!hadDot && n >= 0.0 && n == std::floor(n))) {
            *id = static_cast<std::uint32_t>(n);
            *isConst = false;
        } else {
            *constant = n;
            *isConst = true;
        }
        return true;
    }
    if (!s.empty() && (std::isalpha(static_cast<unsigned char>(s.front())) != 0 || s.front() == '_')) {
        std::size_t n = 0;
        while (n < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[n]);
            if (std::isalnum(c) == 0 && c != '_' && c != '.') {
                break;
            }
            ++n;
        }
        *name = std::string(s.substr(0, n));
        s.remove_prefix(n);
        *isConst = false;
        return true;
    }
    return false;
}

double sampleOrConst(const AnalysisBoundRun& run, bool isConst, double constant, std::uint32_t id, double tMs,
                     bool* ok) {
    if (isConst) {
        *ok = true;
        return constant;
    }
    if (id == 0) {
        *ok = false;
        return 0.0;
    }
    double v = 0.0;
    *ok = run.sampleAt(id, static_cast<std::uint64_t>(std::max(0.0, tMs)), v);
    return v;
}

}  // namespace

bool parseFormulaSpec(std::string_view expr, AnalysisFormulaSpec* out) {
    if (out == nullptr) {
        return false;
    }
    AnalysisFormulaSpec spec;
    spec.expr = std::string(expr);
    std::string_view s = expr;
    skipWs(s);
    if (!parseTerm(s, &spec.leftId, &spec.leftConst, &spec.leftIsConst, &spec.leftName)) {
        return false;
    }
    skipWs(s);
    if (s.empty()) {
        return false;
    }
    const char op = s.front();
    if (op != '+' && op != '-' && op != '*' && op != '/') {
        return false;
    }
    spec.op = op;
    s.remove_prefix(1);
    if (!parseTerm(s, &spec.rightId, &spec.rightConst, &spec.rightIsConst, &spec.rightName)) {
        return false;
    }
    skipWs(s);
    if (!s.empty()) {
        return false;
    }
    *out = std::move(spec);
    return true;
}

void resolveFormulaIds(AnalysisFormulaSpec* spec, const std::vector<ctrl::CatalogSignal>& catalog) {
    if (spec == nullptr) {
        return;
    }
    auto findId = [&](const std::string& name) -> std::uint32_t {
        if (name.empty()) {
            return 0;
        }
        for (const auto& sig : catalog) {
            if (sig.name == name || (!sig.message.empty() && sig.message + "." + sig.name == name)) {
                return sig.id;
            }
        }
        return 0;
    };
    if (spec->leftId == 0 && !spec->leftIsConst) {
        spec->leftId = findId(spec->leftName);
    }
    if (spec->rightId == 0 && !spec->rightIsConst) {
        spec->rightId = findId(spec->rightName);
    }
}

double applyFormulaOp(char op, double a, double b) {
    switch (op) {
        case '+':
            return a + b;
        case '-':
            return a - b;
        case '*':
            return a * b;
        case '/':
            return b == 0.0 ? std::numeric_limits<double>::quiet_NaN() : a / b;
        default:
            return std::numeric_limits<double>::quiet_NaN();
    }
}

AnalysisFormulaResult evaluateFormula(const AnalysisBoundRun& run, const AnalysisFormulaSpec& spec,
                                      std::uint64_t fromMs, std::uint64_t toMs) {
    AnalysisFormulaResult out;
    PlotArrays left;
    if (!spec.leftIsConst && spec.leftId != 0) {
        left = run.gatherL0(spec.leftId, fromMs, toMs);
        if (left.t.empty()) {
            left = run.gatherPlot(spec.leftId, fromMs, toMs, 0);
            out.decimated = left.envelope;
        }
    } else if (spec.leftIsConst && !spec.rightIsConst && spec.rightId != 0) {
        left = run.gatherL0(spec.rightId, fromMs, toMs);
        if (left.t.empty()) {
            left = run.gatherPlot(spec.rightId, fromMs, toMs, 0);
            out.decimated = left.envelope;
        }
        for (double& y : left.y) {
            y = spec.leftConst;
        }
    }
    if (left.t.empty()) {
        return out;
    }
    out.plot.t.reserve(left.t.size());
    out.plot.y.reserve(left.y.size());
    for (std::size_t i = 0; i < left.t.size() && i < left.y.size(); ++i) {
        if (!std::isfinite(left.y[i])) {
            continue;
        }
        bool ok = false;
        const double right = sampleOrConst(run, spec.rightIsConst, spec.rightConst, spec.rightId, left.t[i], &ok);
        if (!ok && !spec.rightIsConst) {
            continue;
        }
        const double y = applyFormulaOp(spec.op, left.y[i], right);
        if (!std::isfinite(y)) {
            continue;
        }
        out.plot.t.push_back(left.t[i]);
        out.plot.y.push_back(y);
    }
    out.plot.envelope = false;
    return out;
}

std::vector<AnalysisEvent> extractThresholdEvents(const AnalysisBoundRun& run, const AnalysisTransform& xf,
                                                  std::uint64_t fromMs, std::uint64_t toMs) {
    std::vector<AnalysisEvent> out;
    if (xf.signalId == 0) {
        return out;
    }
    const auto l0 = run.gatherL0(xf.signalId, fromMs, toMs);
    if (l0.envelope || l0.t.size() < 2) {
        return out;
    }
    const bool isRange = xf.kind == "range";
    const double thr = xf.threshold.value_or(0.0);
    const double lo = xf.min.value_or(thr);
    const double hi = xf.max.value_or(thr);
    std::string edge = xf.spec;
    if (edge.empty()) {
        edge = "rising";
    }
    const bool wantRise = edge == "rising" || edge == "both" || isRange;
    const bool wantFall = edge == "falling" || edge == "both";

    auto inside = [&](double v) {
        if (isRange) {
            return v >= lo && v <= hi;
        }
        return v >= thr;
    };

    for (std::size_t i = 1; i < l0.t.size() && i < l0.y.size(); ++i) {
        if (!std::isfinite(l0.y[i - 1]) || !std::isfinite(l0.y[i])) {
            continue;
        }
        const bool prevIn = inside(l0.y[i - 1]);
        const bool curIn = inside(l0.y[i]);
        if (prevIn == curIn) {
            continue;
        }
        if ((curIn && !wantRise) || (!curIn && !wantFall && !isRange)) {
            continue;
        }
        if (isRange && !curIn) {
            continue;
        }
        AnalysisEvent ev;
        ev.id = xf.id.empty() ? "thr-" + std::to_string(static_cast<unsigned long long>(l0.t[i]))
                              : xf.id + "-" + std::to_string(static_cast<unsigned long long>(l0.t[i]));
        ev.tMs = static_cast<std::uint64_t>(std::max(0.0, l0.t[i]));
        ev.source = kAnalysisEventThreshold;
        ev.signalId = xf.signalId;
        ev.label = xf.label.empty() ? xf.name : xf.label;
        if (ev.label.empty()) {
            ev.label = isRange ? "range" : "threshold";
        }
        ev.value = l0.y[i];
        out.push_back(std::move(ev));
    }
    return out;
}

void replaceThresholdEvents(std::vector<AnalysisEvent>* events, std::vector<AnalysisEvent> extracted) {
    if (events == nullptr) {
        return;
    }
    std::erase_if(*events, [](const AnalysisEvent& ev) { return ev.source == kAnalysisEventThreshold; });
    events->insert(events->end(), std::make_move_iterator(extracted.begin()),
                   std::make_move_iterator(extracted.end()));
}

void collectFormulaSignalIds(const AnalysisFormulaSpec& spec, std::vector<std::uint32_t>* ids) {
    if (ids == nullptr) {
        return;
    }
    auto push = [&](std::uint32_t id) {
        if (id == 0) {
            return;
        }
        if (std::find(ids->begin(), ids->end(), id) == ids->end()) {
            ids->push_back(id);
        }
    };
    if (!spec.leftIsConst) {
        push(spec.leftId);
    }
    if (!spec.rightIsConst) {
        push(spec.rightId);
    }
}

}  // namespace photon::ui
