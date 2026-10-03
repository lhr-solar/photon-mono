#include "photon_ui/client.hpp"
#include "photon_ui/mux.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace photon::ui {
namespace {

constexpr int kChannels = 17;

const ctrl::CatalogSignal* findNamed(const ClientArena& arena, const char* message, const char* name) {
    for (const ctrl::CatalogSignal& cat : arena.signalCatalog) {
        if (cat.message == message && cat.name == name) {
            return &cat;
        }
    }
    return nullptr;
}

void copyRing(const SignalRing* ring, std::vector<double>& t, std::vector<double>& v) {
    t.clear();
    v.clear();
    if (ring == nullptr || ring->count == 0) {
        return;
    }
    t.resize(ring->count);
    v.resize(ring->count);
    for (std::size_t i = 0; i < ring->count; ++i) {
        const std::size_t p = ring->index(i);
        t[i] = ring->t[p];
        v[i] = ring->v[p];
    }
}

struct Channel {
    float volts = 0.0f;
    float amps = 0.0f;
    float enabled = 0.0f;
    float fault = 0.0f;
    double ts = 0.0;
    bool have = false;
};

}  // namespace

void PduScreen::draw(UiContext& ui) {
    const ctrl::CatalogSignal* idx = findNamed(ui.arena, "PDU_Status_Arr", "HSS_Channel_idx");
    const ctrl::CatalogSignal* volts = findNamed(ui.arena, "PDU_Status_Arr", "HSS_Measured_Voltage");
    const ctrl::CatalogSignal* amps = findNamed(ui.arena, "PDU_Status_Arr", "HSS_Measured_Current");
    const ctrl::CatalogSignal* enabled = findNamed(ui.arena, "PDU_Status_Arr", "HSS_Enabled");
    const ctrl::CatalogSignal* fault = findNamed(ui.arena, "PDU_Status_Arr", "HSS_Fault");
    std::vector<std::uint32_t> ids;
    for (const ctrl::CatalogSignal* sig : {idx, volts, amps, enabled, fault}) {
        if (sig != nullptr && sig->id != 0) {
            ids.push_back(sig->id);
        }
    }
    if (!ids.empty()) {
        ui.client.ensureSubscribed(ids);
    }

    ImGui::TextUnformatted("PDU");
    ImGui::SameLine();
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(), "PDU_Status_Arr  channels 0-16");
    theme::popSmallFont();

    if (idx == nullptr || volts == nullptr || amps == nullptr) {
        ImGui::TextWrapped(
            "PDU_Status_Arr is in carcan_telemcan.dbc (voltage, current, enable, fault, channel index). "
            "This session's catalog does not have those signals yet.");
        return;
    }

    std::vector<double> idxT;
    std::vector<double> idxV;
    std::vector<double> voltT;
    std::vector<double> voltV;
    std::vector<double> ampT;
    std::vector<double> ampV;
    std::vector<double> enT;
    std::vector<double> enV;
    std::vector<double> faultT;
    std::vector<double> faultV;
    copyRing(ui.arena.ring(idx->id), idxT, idxV);
    copyRing(ui.arena.ring(volts->id), voltT, voltV);
    copyRing(ui.arena.ring(amps->id), ampT, ampV);
    if (enabled != nullptr) {
        copyRing(ui.arena.ring(enabled->id), enT, enV);
    }
    if (fault != nullptr) {
        copyRing(ui.arena.ring(fault->id), faultT, faultV);
    }

    std::vector<float> voltOut(kChannels, 0.0f);
    std::vector<float> ampOut(kChannels, 0.0f);
    std::vector<float> enOut(kChannels, 0.0f);
    std::vector<float> faultOut(kChannels, 0.0f);
    std::vector<double> voltTs(kChannels, 0.0);
    std::vector<double> ampTs(kChannels, 0.0);
    applyMuxSeries(idxT.data(), idxV.data(), idxT.size(), voltT.data(), voltV.data(), voltT.size(), voltOut,
                   &voltTs, kChannels, true);
    applyMuxSeries(idxT.data(), idxV.data(), idxT.size(), ampT.data(), ampV.data(), ampT.size(), ampOut, &ampTs,
                   kChannels, true);
    if (!enT.empty()) {
        applyMuxSeries(idxT.data(), idxV.data(), idxT.size(), enT.data(), enV.data(), enT.size(), enOut, nullptr,
                       kChannels, true);
    }
    if (!faultT.empty()) {
        applyMuxSeries(idxT.data(), idxV.data(), idxT.size(), faultT.data(), faultV.data(), faultT.size(),
                       faultOut, nullptr, kChannels, true);
    }

    Channel channels[kChannels]{};
    double newest = 0.0;
    for (int i = 0; i < kChannels; ++i) {
        const double ts = std::max(voltTs[static_cast<std::size_t>(i)], ampTs[static_cast<std::size_t>(i)]);
        channels[i].volts = voltOut[static_cast<std::size_t>(i)];
        channels[i].amps = ampOut[static_cast<std::size_t>(i)];
        channels[i].enabled = enOut[static_cast<std::size_t>(i)];
        channels[i].fault = faultOut[static_cast<std::size_t>(i)];
        channels[i].ts = ts;
        channels[i].have = ts > 0.0;
        newest = std::max(newest, ts);
    }

    if (selected_ < 0 || selected_ >= kChannels) {
        selected_ = 0;
    }

    const float gap = theme::kGridGap;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cardW = std::max(108.0f, (avail.x - gap * 8.0f) / 9.0f);
    const float cardH = 86.0f;
    for (int i = 0; i < kChannels; ++i) {
        if (i % 9 != 0) {
            ImGui::SameLine(0.0f, gap);
        }
        ImGui::PushID(i);
        const bool on = channels[i].enabled >= 0.5f;
        const bool bad = channels[i].fault >= 0.5f;
        ImGui::PushStyleColor(ImGuiCol_Button, bad ? theme::withAlpha(theme::danger(), 0.35f)
                                                   : (on ? theme::withAlpha(theme::success(), 0.22f) : theme::card()));
        char label[96];
        if (channels[i].have) {
            std::snprintf(label, sizeof(label), "CH %d\n%.2f V\n%.2f A\n%s", i, channels[i].volts, channels[i].amps,
                          bad ? "FAULT" : (on ? "ON" : "OFF"));
        } else {
            std::snprintf(label, sizeof(label), "CH %d\n--\n--\n", i);
        }
        if (ImGui::Button(label, ImVec2(cardW, cardH))) {
            selected_ = i;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0.0f, gap));
    const Channel& sel = channels[selected_];
    ImGui::Text("Channel %d    %.3f V    %.3f A    %s", selected_, sel.volts, sel.amps,
                sel.have ? (sel.fault >= 0.5f ? "fault" : (sel.enabled >= 0.5f ? "enabled" : "off")) : "no sample");
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(),
                       "Latest sample on each channel. The frame carries one channel index, so the cards fill as "
                       "the PDU walks the channels.");
    theme::popSmallFont();

    std::vector<double> xs(kChannels);
    std::vector<double> ys(kChannels);
    for (int i = 0; i < kChannels; ++i) {
        xs[static_cast<std::size_t>(i)] = static_cast<double>(i);
        ys[static_cast<std::size_t>(i)] = channels[i].have ? static_cast<double>(channels[i].amps) : 0.0;
    }
    theme::pushPlotFont();
    if (ImPlot::BeginPlot("##pdu-amps", ImVec2(-1.0f, std::max(160.0f, ImGui::GetContentRegionAvail().y)),
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes("channel", "A");
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, kChannels - 0.5, ImPlotCond_Always);
        ImPlot::PlotBars("A", xs.data(), ys.data(), kChannels, 0.7);
        ImPlot::EndPlot();
    }
    theme::popPlotFont();
    (void)newest;
}

}  // namespace photon::ui
