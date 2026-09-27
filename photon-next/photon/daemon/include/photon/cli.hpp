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
    std::uint16_t videoUdpPort = 6800;
    std::string videoUdpHost;  // empty = videoHost. Oracle live tee is 10.66.0.1.
    ctrl::SourceKind source = ctrl::SourceKind::DaqTcp;
    std::string logPath;
    std::string tsdbDir;
    bool writeLiveTail = true;
    std::string password;
    // Empty = auto (daq_host:8080 when daq_host is not loopback). "off" disables.
    std::string historyRemote;
};

void usage(const char* argv0);
bool parseU16(const char* text, std::uint16_t& out);
bool parseArgs(int argc, char** argv, Options& opt);

}  // namespace photon::cli
