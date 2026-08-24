#pragma once

#include "photon/control.hpp"

#include <cstdint>
#include <string>

namespace photon::cli {

struct Options {
    std::uint16_t wsPort = 8765;
    std::string daqHost = "127.0.0.1";
    std::uint16_t daqPort = 6500;
    std::string assetsDir;
    std::string videoHost;
    std::uint16_t videoPort = 6700;
    ctrl::SourceKind source = ctrl::SourceKind::Synthetic;
    std::string logPath;
};

void usage(const char* argv0);
bool parseU16(const char* text, std::uint16_t& out);
bool parseArgs(int argc, char** argv, Options& opt);

}  // namespace photon::cli
