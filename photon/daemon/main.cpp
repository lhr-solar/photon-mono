#include "photon/cli.hpp"
#include "photon/history_remote.hpp"
#include "photon/net.hpp"
#include "photon/session.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

volatile std::sig_atomic_t gStop = 0;

void onSignal(int) {
    gStop = 1;
}

std::uint64_t wallMs() {
    using clock = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

}  // namespace

int main(int argc, char** argv) {
    photon::cli::Options opt;
    if (!photon::cli::parseArgs(argc, argv, opt)) {
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    photon::net::Guard sockets;
    if (opt.videoHost.empty()) {
        opt.videoHost = opt.daqHost;
    }

    std::fprintf(stdout, "[photon-backend] photon-next backend v0.1 (protocol v1)\n");
    std::fprintf(stdout, "[photon-backend] flags --ws-port %u --daq-host %s --daq-port %u --video-host %s --video-port %u --video-udp-port %u --source %s",
                 static_cast<unsigned>(opt.wsPort), opt.daqHost.c_str(), static_cast<unsigned>(opt.daqPort),
                 opt.videoHost.c_str(), static_cast<unsigned>(opt.videoPort),
                 static_cast<unsigned>(opt.videoUdpPort),
                 std::string(photon::ctrl::sourceName(opt.source)).c_str());
    if (!opt.logPath.empty()) {
        std::fprintf(stdout, " --log-path %s", opt.logPath.c_str());
    }
    std::fprintf(stdout, "\n");

    photon::SessionHub hub(opt.wsPort, opt.daqHost, opt.daqPort, opt.assetsDir, opt.videoHost, opt.videoPort,
                           opt.source, opt.logPath, opt.tsdbDir, opt.videoUdpPort);
    hub.setWriteLiveTail(opt.writeLiveTail);
    hub.setClientPassword(opt.password);
    if (!opt.videoUdpHost.empty()) {
        hub.setVideoUdpHost(opt.videoUdpHost);
        std::fprintf(stdout, "[photon-backend] video udp host %s\n", opt.videoUdpHost.c_str());
    }
    std::fprintf(stdout, "[photon-backend] client auth %s\n", opt.password.empty() ? "off" : "on");
    hub.setAnalysisCompute(opt.analysisCompute);
    std::string historyHost;
    std::uint16_t historyPort = 0;
    if (opt.historyRemote.empty()) {
        if (!photon::hostIsLoopback(opt.daqHost)) {
            historyHost = opt.daqHost;
            historyPort = 8080;
        }
    } else if (!photon::parseHostPort(opt.historyRemote, historyHost, historyPort)) {
        std::fprintf(stderr, "[photon-backend] --history-remote must be HOST:PORT or off\n");
        return 2;
    }
    if (!historyHost.empty()) {
        hub.setHistoryRemote(historyHost, historyPort, "/ws", opt.password);
        std::fprintf(stdout, "[photon-backend] history-remote %s:%u/ws\n", historyHost.c_str(),
                     static_cast<unsigned>(historyPort));
    } else {
        std::fprintf(stdout, "[photon-backend] history-remote off\n");
    }
    if (opt.analysisCompute.empty()) {
        std::fprintf(stdout, "[photon-backend] analysis-compute off\n");
    } else {
        std::fprintf(stdout, "[photon-backend] analysis-compute %s\n", opt.analysisCompute.c_str());
    }
    if (!hub.start()) {
        return 1;
    }

    while (gStop == 0) {
        hub.tick(wallMs());
    }

    std::fprintf(stdout, "[photon-backend] shutting down\n");
    hub.stop();
    return 0;
}
