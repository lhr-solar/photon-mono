#include "photon/cli.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

void runCliTests();

namespace {

bool parse(std::vector<std::string> args, photon::cli::Options& opt) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& s : args) {
        argv.push_back(s.data());
    }
    return photon::cli::parseArgs(static_cast<int>(argv.size()), argv.data(), opt);
}

}  // namespace

void runCliTests() {
    photon::cli::Options def;
    CHECK(parse({"photon-backend"}, def));
    CHECK(def.source == photon::ctrl::SourceKind::Synthetic);
    CHECK(def.wsPort == 8765);
    CHECK(def.logPath.empty());

    photon::cli::Options replay;
    CHECK(parse({"photon-backend", "--source", "log_replay", "--log-path",
                 R"(C:\Users\alper\Downloads\logs\logs)", "--ws-port", "8790"},
                replay));
    CHECK(replay.source == photon::ctrl::SourceKind::LogReplay);
    CHECK(replay.logPath == R"(C:\Users\alper\Downloads\logs\logs)");
    CHECK(replay.wsPort == 8790);

    photon::cli::Options daq;
    CHECK(parse({"photon-backend", "--source", "daq_tcp", "--daq-host", "10.0.0.2", "--daq-port", "6501"},
                daq));
    CHECK(daq.source == photon::ctrl::SourceKind::DaqTcp);
    CHECK(daq.daqHost == "10.0.0.2");
    CHECK(daq.daqPort == 6501);

    photon::cli::Options pcan;
    CHECK(parse({"photon-backend", "--source", "pcan"}, pcan));
    CHECK(pcan.source == photon::ctrl::SourceKind::Pcan);

    photon::cli::Options missingPath;
    CHECK(!parse({"photon-backend", "--source", "log_replay"}, missingPath));

    photon::cli::Options badKind;
    CHECK(!parse({"photon-backend", "--source", "nope"}, badKind));

    photon::cli::Options unknown;
    CHECK(!parse({"photon-backend", "--not-a-flag"}, unknown));
}
