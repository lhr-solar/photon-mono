#pragma once

#include "photon_ui/platform.hpp"

#include <string>
#include <string_view>

namespace photon::ui {

// Live link for this page: protocol v2 at the page's own /v2 (web), or the
// native default ws://127.0.0.1:6900/v2 (--ws / PHOTON_WS override it).
std::string pageLiveUrl();

// Web: the LAN password remembered in this browser, and a prompt that asks for
// it and remembers the answer. Native: the --password value; asking returns "".
std::string pageStoredToken();
std::string pageAskToken();

// True when the page is opened from another machine (not loopback).
bool pageIsSharedConsole();

// Hostname the browser used (sslip.io → the IP). Empty on file://.
std::string pagePublicHost();

}  // namespace photon::ui
