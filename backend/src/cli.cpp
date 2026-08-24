#include "photon/cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace photon::cli {

void usage(const char* argv0) {
    std::fprintf(stdout,
                 "Usage: %s [--ws-port N] [--daq-host HOST] [--daq-port N] [--video-host HOST] "
                 "[--video-port N] [--assets-dir DIR] [--source KIND] [--log-path PATH]\n"
                 "  --ws-port     WebSocket listen port (default 8765, bound to 127.0.0.1)\n"
                 "  --daq-host    Default CANP fan-out host (default 127.0.0.1)\n"
                 "  --daq-port    Default CANP fan-out port (default 6500)\n"
                 "  --video-host  photon-server video host (default: same as --daq-host)\n"
                 "  --video-port  photon-server video TCP port (default 6700)\n"
                 "  --assets-dir  Track JSON root (default: compile-time assets/ or discovery)\n"
                 "  --source      synthetic|daq_tcp|log_replay|pcan (default synthetic)\n"
                 "  --log-path    VSD1 .log file or segment directory (required for log_replay)\n",
                 argv0);
}

bool parseU16(const char* text, std::uint16_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || v > 65535ul) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto need = [&](std::uint16_t& dest) -> bool {
            if (i + 1 >= argc || !parseU16(argv[++i], dest)) {
                std::fprintf(stderr, "[photon-backend] invalid value for %s\n", arg.c_str());
                return false;
            }
            return true;
        };
        const auto needStr = [&](std::string& dest, const char* flag) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[photon-backend] %s requires a value\n", flag);
                return false;
            }
            dest = argv[++i];
            return true;
        };
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        if (arg == "--ws-port") {
            if (!need(opt.wsPort)) {
                return false;
            }
        } else if (arg == "--daq-host") {
            if (!needStr(opt.daqHost, "--daq-host")) {
                return false;
            }
        } else if (arg == "--daq-port") {
            if (!need(opt.daqPort)) {
                return false;
            }
        } else if (arg == "--video-host") {
            if (!needStr(opt.videoHost, "--video-host")) {
                return false;
            }
        } else if (arg == "--video-port") {
            if (!need(opt.videoPort)) {
                return false;
            }
        } else if (arg == "--assets-dir") {
            if (!needStr(opt.assetsDir, "--assets-dir")) {
                return false;
            }
        } else if (arg == "--source") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[photon-backend] --source requires a value\n");
                return false;
            }
            const auto kind = ctrl::sourceFromName(argv[++i]);
            if (!kind) {
                std::fprintf(stderr,
                             "[photon-backend] --source must be synthetic|daq_tcp|log_replay|pcan\n");
                return false;
            }
            opt.source = *kind;
        } else if (arg == "--log-path") {
            if (!needStr(opt.logPath, "--log-path")) {
                return false;
            }
        } else {
            std::fprintf(stderr, "[photon-backend] unknown flag %s\n", arg.c_str());
            usage(argv[0]);
            return false;
        }
    }
    if (opt.source == ctrl::SourceKind::LogReplay && opt.logPath.empty()) {
        std::fprintf(stderr, "[photon-backend] --source log_replay requires --log-path\n");
        return false;
    }
    return true;
}

}  // namespace photon::cli
