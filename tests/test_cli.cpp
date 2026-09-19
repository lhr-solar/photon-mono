#include "test_harness.hpp"
#include "photon/cli.hpp"
#include "photon/history_remote.hpp"

#ifdef near
#undef near
#endif

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
    CHECK(def.source == photon::ctrl::SourceKind::DaqTcp);
    CHECK(def.wsPort == 8765);
    CHECK(def.logPath.empty());
    CHECK(def.writeLiveTail);
    CHECK(def.videoUdpPort == 6800);
    CHECK(def.analysisCompute.empty());
    CHECK(def.historyRemote.empty());

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
    CHECK(parse({"photon-backend", "--source", "log_replay"}, missingPath));
    CHECK(missingPath.source == photon::ctrl::SourceKind::LogReplay);
    CHECK(missingPath.logPath.empty());

    photon::cli::Options badKind;
    CHECK(!parse({"photon-backend", "--source", "nope"}, badKind));

    photon::cli::Options noSynth;
    CHECK(!parse({"photon-backend", "--source", "synthetic"}, noSynth));

    photon::cli::Options unknown;
    CHECK(!parse({"photon-backend", "--not-a-flag"}, unknown));

    photon::cli::Options tsdb;
    CHECK(parse({"photon-backend", "--tsdb-dir", R"(F:\lhr\data\tsdb)"}, tsdb));
    CHECK(tsdb.tsdbDir == R"(F:\lhr\data\tsdb)");
    CHECK(tsdb.writeLiveTail);

    photon::cli::Options laptop;
    CHECK(parse({"photon-backend", "--no-live-tail", "--daq-host", "192.168.1.10"}, laptop));
    CHECK(!laptop.writeLiveTail);
    CHECK(laptop.daqHost == "192.168.1.10");

    photon::cli::Options udpOff;
    CHECK(parse({"photon-backend", "--video-udp-port", "0"}, udpOff));
    CHECK(udpOff.videoUdpPort == 0);

    photon::cli::Options secret;
    CHECK(parse({"photon-backend", "--password", "pit"}, secret));
    CHECK(secret.password == "pit");

    photon::cli::Options computeOff;
    CHECK(parse({"photon-backend", "--analysis-compute", "off"}, computeOff));
    CHECK(computeOff.analysisCompute.empty());

    photon::cli::Options computeOn;
    CHECK(parse({"photon-backend", "--analysis-compute", "127.0.0.1:18765"}, computeOn));
    CHECK(computeOn.analysisCompute == "127.0.0.1:18765");

    photon::cli::Options computeEq;
    CHECK(parse({"photon-backend", "--analysis-compute=127.0.0.1:18765"}, computeEq));
    CHECK(computeEq.analysisCompute == "127.0.0.1:18765");

    photon::cli::Options computeLan;
    CHECK(!parse({"photon-backend", "--analysis-compute", "0.0.0.0:18765"}, computeLan));
    photon::cli::Options computeIngest;
    CHECK(!parse({"photon-backend", "--analysis-compute", "10.0.0.2:6100"}, computeIngest));

    photon::cli::Options histOff;
    CHECK(parse({"photon-backend", "--history-remote", "off"}, histOff));
    CHECK(histOff.historyRemote == "off");

    photon::cli::Options histOn;
    CHECK(parse({"photon-backend", "--history-remote", "150.136.85.207:8080"}, histOn));
    CHECK(histOn.historyRemote == "150.136.85.207:8080");

    photon::cli::Options histEq;
    CHECK(parse({"photon-backend", "--history-remote=150.136.85.207:8080"}, histEq));
    CHECK(histEq.historyRemote == "150.136.85.207:8080");

    photon::cli::Options histBad;
    CHECK(!parse({"photon-backend", "--history-remote", "noport"}, histBad));

    std::string host;
    std::uint16_t port = 0;
    CHECK(photon::parseHostPort("off", host, port));
    CHECK(host.empty());
    CHECK(photon::parseHostPort("150.136.85.207:8080", host, port));
    CHECK(host == "150.136.85.207");
    CHECK(port == 8080);
    CHECK(photon::hostIsLoopback("127.0.0.1"));
    CHECK(photon::hostIsLoopback("localhost"));
    CHECK(!photon::hostIsLoopback("150.136.85.207"));
}
