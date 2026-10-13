#pragma once

#include "ui/platform.hpp"

#include <string>
#include <string_view>

namespace photon::ui {

// Live link for this page. Public Oracle / photon-server UI uses same-origin
// `/ws`. Local emrun (odd ports) still talks to loopback :8765.
std::string pageLiveUrl();

// True when the page is opened from another machine (not loopback).
bool pageIsSharedConsole();

// Hostname the browser used (sslip.io → the IP). Empty on file://.
std::string pagePublicHost();

// MACBOOK fan-out is 127.0.0.1:6500. On the Oracle page, show the pipe instead.
std::string sourceDisplayHost(std::string_view backendHost);

}  // namespace photon::ui
