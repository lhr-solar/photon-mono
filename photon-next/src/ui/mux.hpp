#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace photon::ui {

// Demux BPS_Tap_idx + tap data into a 32-slot pack.
//
// CANP batches stamp every frame with the same millisecond, so pairing by
// timestamp alone keeps only the last tap in each batch. Same-length series
// are paired by sample order (DDash). Otherwise each timestamp keeps a queue
// of taps so every tap in that millisecond still lands.
inline void applyMuxSeries(const double* idxT, const double* idxV, std::size_t idxN, const double* valT,
                           const double* valV, std::size_t valN, std::vector<float>& out,
                           std::vector<double>* tapTs, int count, bool reset) {
    if (count <= 0 || count > 256) {
        out.clear();
        if (tapTs != nullptr) {
            tapTs->clear();
        }
        return;
    }
    const auto n = static_cast<std::size_t>(count);
    if (reset || out.size() != n) {
        out.assign(n, 0.0f);
    }
    if (tapTs != nullptr && (reset || tapTs->size() != n)) {
        tapTs->assign(n, 0.0);
    }
    if (idxT == nullptr || idxV == nullptr || valT == nullptr || valV == nullptr || idxN == 0 || valN == 0) {
        return;
    }

    std::vector<char> seen(n, 0);
    auto writeSlot = [&](int tap, double value, double t) {
        if (tap < 0 || tap >= count || !std::isfinite(value)) {
            return;
        }
        const auto slot = static_cast<std::size_t>(tap);
        if (seen[slot] != 0) {
            return;
        }
        out[slot] = static_cast<float>(value);
        if (tapTs != nullptr) {
            (*tapTs)[slot] = t;
        }
        seen[slot] = 1;
    };

    bool lockstep = idxN == valN;
    if (lockstep) {
        for (std::size_t i = 0; i < idxN; ++i) {
            if (static_cast<std::uint64_t>(idxT[i]) != static_cast<std::uint64_t>(valT[i])) {
                lockstep = false;
                break;
            }
        }
    }
    if (lockstep) {
        for (std::size_t nSeen = idxN; nSeen > 0; --nSeen) {
            const std::size_t i = nSeen - 1;
            writeSlot(static_cast<int>(std::lround(idxV[i])), valV[i], valT[i]);
        }
        return;
    }

    std::unordered_map<std::uint64_t, std::vector<int>> tapsByTs;
    tapsByTs.reserve(idxN);
    for (std::size_t i = 0; i < idxN; ++i) {
        const int tap = static_cast<int>(std::lround(idxV[i]));
        if (tap < 0 || tap >= count) {
            continue;
        }
        tapsByTs[static_cast<std::uint64_t>(idxT[i])].push_back(tap);
    }
    for (std::size_t nSeen = valN; nSeen > 0; --nSeen) {
        const std::size_t i = nSeen - 1;
        const auto it = tapsByTs.find(static_cast<std::uint64_t>(valT[i]));
        if (it == tapsByTs.end() || it->second.empty()) {
            continue;
        }
        const int tap = it->second.back();
        it->second.pop_back();
        writeSlot(tap, valV[i], valT[i]);
    }
}

}  // namespace photon::ui
