#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace photon::ui {

class HistoryIO {
public:
    virtual ~HistoryIO() = default;
    virtual void requestHistory(std::uint32_t queryId, std::string sessionId, std::uint64_t fromMs,
                                std::uint64_t toMs, std::vector<std::uint32_t> signalIds,
                                std::uint32_t maxPoints) = 0;
    virtual void cancelHistory(std::uint32_t queryId) = 0;
};

}  // namespace photon::ui
