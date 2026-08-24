#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"
#include "photon_ui/video.hpp"

#include "imgui.h"

#include <cstdio>

namespace photon::ui {

void LiveViewScreen::draw(UiContext& ui) {
    videoDecoder().poll();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float barH = 44.0f;
    const float statusH = 36.0f;
    const float gap = theme::kGridGap;

    theme::beginCard("livebar", ImVec2(avail.x, barH), theme::card(), theme::kStripPadding,
                     theme::CardFit::Band);
    if (ImGui::Button("Live", ImVec2(72.0f, 0.0f))) {
        ui.client.subscribeVideo(ctrl::VideoMode::Live, 0);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputScalar("##replay_ms", ImGuiDataType_U64, &replayMs_);
    ImGui::SameLine();
    if (ImGui::Button("Go", ImVec2(48.0f, 0.0f))) {
        ui.client.subscribeVideo(ctrl::VideoMode::Replay, replayMs_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(64.0f, 0.0f))) {
        ui.client.stopVideo();
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "replay start_ms");
    theme::endCard();

    const auto& v = ui.arena.video;
    theme::beginCard("livestat", ImVec2(avail.x, statusH), theme::muted(), theme::kStripPadding,
                     theme::CardFit::Band);
    if (!videoDecoder().supported()) {
        ImGui::TextColored(theme::warning(), "WebCodecs unsupported in this browser");
    } else if (!v.haveStatus && !v.subscribed) {
        ImGui::TextColored(theme::mutedFg(), "Not subscribed — press Live");
    } else if (v.haveStatus && !v.connected) {
        ImGui::TextColored(theme::warning(), "No video server at %s:%u", v.host.c_str(),
                           static_cast<unsigned>(v.port));
    } else {
        ImGui::Text("%s  mode=%s  fps=%0.1f  frames=%llu  drops=%llu  last_ts=%llu",
                    v.connected ? "connected" : "disconnected", v.mode.c_str(), v.fps,
                    static_cast<unsigned long long>(v.frames), static_cast<unsigned long long>(v.dropped),
                    static_cast<unsigned long long>(v.lastTsMs));
    }
    if (videoDecoder().error()[0] != '\0') {
        ImGui::SameLine();
        ImGui::TextColored(theme::danger(), "%s", videoDecoder().error());
    }
    theme::endCard();

    const float videoH = avail.y - barH - statusH - gap * 2.0f;
    theme::beginCard("livevideo", ImVec2(avail.x, videoH), theme::background(), theme::CardFit::Band);
    drawVideoImage(ImGui::GetContentRegionAvail());
    theme::endCard();
}

}  // namespace photon::ui
