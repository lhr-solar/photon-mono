#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"

namespace photon::ui {

void PlotsScreen::draw(UiContext& ui) { views_.draw(ui); }

void PlotsScreen::onPlayOrLive() { views_.releasePlayheadScrub(); }

void PlotsScreen::onSessionChanged() { views_.followSessionWindow(); }

}  // namespace photon::ui
