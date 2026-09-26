#pragma once

#include "photon/net.hpp"

#include <string>
#include <string_view>

namespace photon {

void handleCsvExport(net::Socket sock, std::string_view target, const std::string& tsdbDir);

}  // namespace photon
