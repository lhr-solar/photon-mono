#include "photon_ui/analysis_runtime.hpp"

#include <algorithm>
#include <utility>

namespace photon::ui {

namespace {
// The recorded lists are for tests and debugging; a long session must not grow them.
constexpr std::size_t kMaxRecorded = 1024;
}  // namespace

void AnalysisHistoryIO::requestTile(std::uint32_t queryId, const std::string& runId, std::uint32_t signalId,
                                    int level, std::int64_t index, bool urgent) {
    requests_.push_back(AnalysisTileRequest{queryId, runId, signalId, level, index, urgent});
    if (requests_.size() > kMaxRecorded) {
        requests_.erase(requests_.begin());
    }
    if (forward_ != nullptr && forward_ != this) {
        forward_->requestTile(queryId, runId, signalId, level, index, urgent);
    }
}

void AnalysisHistoryIO::cancelTile(std::uint32_t queryId) {
    cancels_.push_back(queryId);
    if (cancels_.size() > kMaxRecorded) {
        cancels_.erase(cancels_.begin());
    }
    if (forward_ != nullptr && forward_ != this) {
        forward_->cancelTile(queryId);
    }
}

bool AnalysisHistoryIO::queueFull() const {
    return forward_ != nullptr && forward_ != this && forward_->queueFull();
}

AnalysisBoundRun::AnalysisBoundRun(AnalysisRunBinding binding, std::uint32_t queryOrigin)
    : binding_(std::move(binding)), queryOrigin_(queryOrigin) {
    cache_.setTileStore(nullptr);
    cache_.setQueryOrigin(queryOrigin_);
    applySession();
}

void AnalysisBoundRun::rebind(AnalysisRunBinding binding) {
    cache_.cancelAll(io_);
    binding_ = std::move(binding);
    cache_.setQueryOrigin(queryOrigin_);
    applySession();
}

void AnalysisBoundRun::setTimeOffsetMs(std::int64_t ms) {
    binding_.timeOffsetMs = ms;
}

void AnalysisBoundRun::setColor(std::string color) {
    binding_.color = std::move(color);
}

void AnalysisBoundRun::applySession() {
    cache_.setRun(binding_.sessionId, binding_.rev, binding_.dbcHash, binding_.minMs, binding_.maxMs);
}

void AnalysisBoundRun::setRev(std::int64_t rev) {
    if (binding_.rev == rev) {
        return;
    }
    cache_.cancelAll(io_);
    binding_.rev = rev;
    applySession();
}

void AnalysisBoundRun::setLevels(const std::vector<std::int64_t>& bucketMs) {
    cache_.setLevels(bucketMs);
}

void AnalysisBoundRun::requestWindow(std::span<const std::uint32_t> signalIds, std::uint64_t fromMs,
                                     std::uint64_t toMs, std::uint32_t maxPoints, bool dragging) {
    cache_.requestWindow(io_, signalIds, fromMs, toMs, maxPoints, dragging);
}

void AnalysisBoundRun::requestCursorWindow(std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs) {
    cache_.requestCursorWindow(io_, signalIds, cursorMs);
}

void AnalysisBoundRun::cancelLeaving(std::uint64_t fromMs, std::uint64_t toMs) {
    cache_.cancelLeaving(io_, fromMs, toMs);
}

PlotArrays AnalysisBoundRun::gatherPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                                        std::uint32_t maxPoints) const {
    return cache_.gatherPlot(signalId, fromMs, toMs, maxPoints);
}

PlotArrays AnalysisBoundRun::gatherL0(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const {
    return cache_.gatherL0(signalId, fromMs, toMs);
}

bool AnalysisBoundRun::sampleAt(std::uint32_t signalId, std::uint64_t tMs, double& value) const {
    return cache_.sampleAt(signalId, tMs, value);
}


AnalysisBoundRun* AnalysisRuntime::bind(AnalysisRunBinding binding) {
    if (binding.sessionId.empty()) {
        return nullptr;
    }
    if (binding.role == AnalysisRunRole::Overlay) {
        if (const auto* p = primary(); p != nullptr && p->binding().sessionId == binding.sessionId) {
            return nullptr;
        }
    }
    // The same run already bound in the other role leaves that role: it is replaced below.
    const auto sameRunOtherRole = [&binding](const std::unique_ptr<AnalysisBoundRun>& run) {
        return run->binding().sessionId == binding.sessionId && run->binding().role != binding.role;
    };
    std::erase_if(runs_, [&](const std::unique_ptr<AnalysisBoundRun>& run) {
        if (!sameRunOtherRole(run)) {
            return false;
        }
        run->cache().cancelAll(run->io());
        return true;
    });
    if (auto* existing = find(binding.sessionId)) {
        existing->rebind(std::move(binding));
        if (existing->binding().role == AnalysisRunRole::Primary) {
            const auto maxMs = existing->binding().maxMs;
            const auto minMs = existing->binding().minMs;
            if (cursorMs_ < minMs || cursorMs_ > maxMs || cursorMs_ == 0) {
                cursorMs_ = maxMs == 0 ? minMs : maxMs;
            }
        }
        syncBudgets();
        return existing;
    }
    // A new primary (or overlay) replaces the one it had: its requests are cancelled first.
    std::erase_if(runs_, [&binding](const std::unique_ptr<AnalysisBoundRun>& run) {
        if (run->binding().role != binding.role) {
            return false;
        }
        run->cache().cancelAll(run->io());
        return true;
    });
    const std::uint32_t origin = nextQueryOrigin_;
    nextQueryOrigin_ += kAnalysisQueryStride;
    auto run = std::make_unique<AnalysisBoundRun>(std::move(binding), origin);
    if (transport_ != nullptr) {
        run->io().setForward(transport_);
    }
    run->setLevels(levels_);
    auto* ptr = run.get();
    if (ptr->binding().role == AnalysisRunRole::Primary) {
        const auto maxMs = ptr->binding().maxMs;
        const auto minMs = ptr->binding().minMs;
        if (cursorMs_ < minMs || cursorMs_ > maxMs || cursorMs_ == 0) {
            cursorMs_ = maxMs == 0 ? minMs : maxMs;
        }
        if (windowMs_ == 0) {
            windowMs_ = 15000;
        }
        if (maxMs > minMs) {
            const auto span = maxMs - minMs;
            if (windowMs_ > span) {
                windowMs_ = span;
            }
        }
    }
    runs_.push_back(std::move(run));
    syncBudgets();
    haveHistWindow_ = false;
    return ptr;
}

void AnalysisRuntime::unbind(std::string_view sessionId) {
    std::erase_if(runs_, [&](const std::unique_ptr<AnalysisBoundRun>& run) {
        return run->binding().sessionId == sessionId;
    });
    syncBudgets();
}

void AnalysisRuntime::clear() {
    runs_.clear();
    haveHistWindow_ = false;
}

const AnalysisBoundRun* AnalysisRuntime::findConst(std::string_view sessionId) const {
    for (const auto& run : runs_) {
        if (run->binding().sessionId == sessionId) {
            return run.get();
        }
    }
    return nullptr;
}

AnalysisBoundRun* AnalysisRuntime::find(std::string_view sessionId) {
    return const_cast<AnalysisBoundRun*>(findConst(sessionId));
}

const AnalysisBoundRun* AnalysisRuntime::find(std::string_view sessionId) const {
    return findConst(sessionId);
}

AnalysisBoundRun* AnalysisRuntime::primary() {
    return const_cast<AnalysisBoundRun*>(static_cast<const AnalysisRuntime*>(this)->primary());
}

const AnalysisBoundRun* AnalysisRuntime::primary() const {
    for (const auto& run : runs_) {
        if (run->binding().role == AnalysisRunRole::Primary) {
            return run.get();
        }
    }
    return nullptr;
}

AnalysisBoundRun* AnalysisRuntime::overlay() {
    return const_cast<AnalysisBoundRun*>(static_cast<const AnalysisRuntime*>(this)->overlay());
}

const AnalysisBoundRun* AnalysisRuntime::overlay() const {
    for (const auto& run : runs_) {
        if (run->binding().role == AnalysisRunRole::Overlay) {
            return run.get();
        }
    }
    return nullptr;
}

void AnalysisRuntime::setOverlayOffsetMs(std::int64_t ms) {
    if (auto* run = overlay()) {
        run->setTimeOffsetMs(ms);
    }
}

std::int64_t AnalysisRuntime::overlayOffsetMs() const {
    if (const auto* run = overlay()) {
        return run->binding().timeOffsetMs;
    }
    return 0;
}

std::size_t AnalysisRuntime::residentBytes() const {
    std::size_t n = 0;
    for (const auto& run : runs_) {
        n += run->cache().residentBytes();
    }
    return n;
}

std::size_t AnalysisRuntime::overshootBytes() const {
    std::size_t n = 0;
    for (const auto& run : runs_) {
        n += run->cache().overshootBytes();
    }
    return n;
}

void AnalysisRuntime::setBudget(std::size_t bytes) {
    budget_ = bytes == 0 ? TileCache::kDefaultBudget : bytes;
    syncBudgets();
}

void AnalysisRuntime::syncBudgets() {
    if (runs_.empty()) {
        return;
    }
    const std::size_t each = std::max<std::size_t>(budget_ / runs_.size(), 1024 * 1024);
    for (auto& run : runs_) {
        run->cache().setBudget(each);
    }
}

void AnalysisRuntime::setTransport(HistoryIO* io) {
    transport_ = io;
    for (auto& run : runs_) {
        run->io().setForward(io);
    }
}

void AnalysisRuntime::requestWindow(std::span<const std::uint32_t> signalIds, std::uint64_t fromMs,
                                    std::uint64_t toMs, std::uint32_t maxPoints, bool dragging) {
    for (auto& run : runs_) {
        auto from = fromMs;
        auto to = toMs;
        if (run->binding().role == AnalysisRunRole::Overlay) {
            from = analysisNativeMs(fromMs, run->binding().timeOffsetMs);
            to = analysisNativeMs(toMs, run->binding().timeOffsetMs);
            if (to < from) {
                std::swap(from, to);
            }
        }
        run->requestWindow(signalIds, from, to, maxPoints, dragging);
    }
}

void AnalysisRuntime::requestCursorWindow(std::span<const std::uint32_t> signalIds, std::uint64_t cursorMs) {
    for (auto& run : runs_) {
        auto t = cursorMs;
        if (run->binding().role == AnalysisRunRole::Overlay) {
            t = analysisNativeMs(cursorMs, run->binding().timeOffsetMs);
        }
        run->requestCursorWindow(signalIds, t);
    }
}

void AnalysisRuntime::cancelLeaving(std::uint64_t fromMs, std::uint64_t toMs) {
    for (auto& run : runs_) {
        auto from = fromMs;
        auto to = toMs;
        if (run->binding().role == AnalysisRunRole::Overlay) {
            from = analysisNativeMs(fromMs, run->binding().timeOffsetMs);
            to = analysisNativeMs(toMs, run->binding().timeOffsetMs);
            if (to < from) {
                std::swap(from, to);
            }
        }
        run->cancelLeaving(from, to);
    }
}

void AnalysisRuntime::tickHistory(std::span<const std::uint32_t> signalIds, std::uint32_t maxPoints) {
    if (!hasBoundRun() || signalIds.empty()) {
        return;
    }
    const auto fromMs = windowFromMs();
    const auto toMs = windowToMs();
    if (haveHistWindow_ && (fromMs != histFromMs_ || toMs != histToMs_)) {
        cancelLeaving(fromMs, toMs);
    }
    haveHistWindow_ = true;
    histFromMs_ = fromMs;
    histToMs_ = toMs;
    if (scrubbing_) {
        return;
    }
    requestWindow(signalIds, fromMs, toMs, maxPoints, false);
    requestCursorWindow(signalIds, displayCursorMs());
}

PlotArrays AnalysisRuntime::gatherPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                                       std::uint32_t maxPoints) const {
    if (const auto* run = primary()) {
        return run->gatherPlot(signalId, fromMs, toMs, maxPoints);
    }
    return {};
}

PlotArrays AnalysisRuntime::gatherOverlayPlot(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                                              std::uint32_t maxPoints) const {
    const auto* run = overlay();
    if (run == nullptr) {
        return {};
    }
    const auto off = run->binding().timeOffsetMs;
    auto from = analysisNativeMs(fromMs, off);
    auto to = analysisNativeMs(toMs, off);
    if (to < from) {
        std::swap(from, to);
    }
    auto plot = run->gatherPlot(signalId, from, to, maxPoints);
    applyRenderOffset(plot, off);
    return plot;
}

PlotArrays AnalysisRuntime::gatherL0(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const {
    if (const auto* run = primary()) {
        return run->gatherL0(signalId, fromMs, toMs);
    }
    return {};
}

void AnalysisRuntime::seekCursor(std::uint64_t tMs) {
    releaseView();
    setCursorMs(tMs);
}

bool AnalysisRuntime::sampleAt(std::uint32_t signalId, std::uint64_t tMs, double& value) const {
    if (const auto* run = primary()) {
        return run->sampleAt(signalId, tMs, value);
    }
    return false;
}

void AnalysisRuntime::setLevels(const std::vector<std::int64_t>& bucketMs) {
    levels_ = bucketMs;
    for (auto& run : runs_) {
        run->setLevels(levels_);
    }
}

void AnalysisRuntime::applyRuns(const std::vector<ctrl2::Run>& runs) {
    for (auto& run : runs_) {
        for (const auto& r : runs) {
            if (r.id == run->binding().sessionId) {
                run->setRev(r.rev);
                break;
            }
        }
    }
}

void AnalysisRuntime::abandonRequests() {
    for (auto& run : runs_) {
        run->abandonRequests();
    }
}

void AnalysisRuntime::pump() {
    for (auto& run : runs_) {
        run->pump();
    }
}

TileCache* AnalysisRuntime::cacheForQuery(std::uint32_t qid) {
    for (auto& run : runs_) {
        if (run->cache().owns(qid)) {
            return &run->cache();
        }
    }
    return nullptr;
}

void AnalysisRuntime::setCursorMs(std::uint64_t cursorMs) {
    cursorMs_ = cursorMs;
    if (!scrubbing_) {
        previewMs_ = cursorMs;
    }
}

void AnalysisRuntime::setWindowMs(std::uint64_t windowMs) {
    windowMs_ = windowMs == 0 ? 15000 : windowMs;
}

void AnalysisRuntime::beginScrub() {
    scrubbing_ = true;
    previewMs_ = cursorMs_;
}

void AnalysisRuntime::previewScrub(std::uint64_t cursorMs) {
    if (!scrubbing_) {
        beginScrub();
    }
    previewMs_ = cursorMs;
}

void AnalysisRuntime::commitScrub() {
    if (scrubbing_) {
        cursorMs_ = previewMs_;
        scrubbing_ = false;
    }
}

void AnalysisRuntime::cancelScrub() {
    scrubbing_ = false;
    previewMs_ = cursorMs_;
}

void AnalysisRuntime::holdView(std::uint64_t fromMs, std::uint64_t toMs) {
    if (toMs < fromMs) {
        std::swap(fromMs, toMs);
    }
    if (toMs == fromMs) {
        toMs = fromMs + (windowMs_ == 0 ? 15000 : windowMs_);
    }
    viewFromMs_ = fromMs;
    viewToMs_ = toMs;
    viewHeld_ = true;
}

void AnalysisRuntime::releaseView() {
    viewHeld_ = false;
}

std::uint64_t AnalysisRuntime::windowFromMs() const {
    if (viewHeld_) {
        return viewFromMs_;
    }
    const auto end = windowToMs();
    if (end <= windowMs_) {
        return 0;
    }
    return end - windowMs_;
}

std::uint64_t AnalysisRuntime::windowToMs() const {
    if (viewHeld_) {
        return viewToMs_;
    }
    if (displayCursorMs() != 0) {
        return displayCursorMs();
    }
    if (const auto* run = primary()) {
        return run->binding().maxMs;
    }
    return 0;
}

}  // namespace photon::ui
