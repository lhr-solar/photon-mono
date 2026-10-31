#include "ui/client.hpp"
#include "ui/screen.hpp"

namespace photon::ui {

void PlotsScreen::draw(UiContext& ui) { views_.draw(ui); }

void PlotsScreen::onPlayOrLive() { views_.releasePlayheadScrub(); }

void PlotsScreen::onSessionChanged() { views_.followSessionWindow(); }

}  // namespace photon::ui
