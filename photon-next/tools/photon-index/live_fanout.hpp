#pragma once

#include <atomic>
#include <cstdint>
#include <string>

struct LiveFanoutOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6500;
    std::string outDir;
    std::string dbcPath;
    std::string videoVptPath;
    std::string password;
    // 0 = run until disconnect / stop. Tests pass a small count.
    std::uint64_t maxFrames = 0;
    std::atomic<bool>* stop = nullptr;
};

// Connects to photon-server CANP fan-out and writes the TSDB live.
// Reconnects until stop or maxFrames. Returns 0 on a clean stop.
int runLiveFanout(const LiveFanoutOptions& opt);
