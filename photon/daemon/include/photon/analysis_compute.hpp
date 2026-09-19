#pragma once

#include "photon/control.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace photon::analysis {

inline constexpr const char* kOfflineCode = "analysis_offline";
inline constexpr const char* kBusyCode = "analysis_busy";
inline constexpr int kMaxInflightPerClient = 1;
inline constexpr int kMaxInflightGlobal = 2;
inline constexpr int kDefaultTimeoutS = 120;

struct Addr {
    std::string host = "127.0.0.1";
    std::uint16_t port = 18765;
};

struct JobSpec {
    std::string jobId;
    std::string clientId;
    std::string sessionId;
    std::string dbcHash;
    std::vector<std::uint32_t> signalIds;
    std::uint64_t t0Ms = 0;
    std::uint64_t t1Ms = 0;
    std::uint32_t maxPoints = 2000;
    std::string prompt;
    std::string recipeId;
    std::string kind = "grok";
    int timeoutS = kDefaultTimeoutS;
};

bool parseAddr(std::string_view text, Addr& out);

ctrl::Message offlineResult(std::string jobId, std::string message = "analysis compute sidecar is not running");
ctrl::Message busyResult(std::string jobId);

// Metadata-only bundle (no TSDB open). Used by the scaffold; WP-I fills series.
bool writeStubBundle(const JobSpec& spec, std::string& pathOut, std::string* error = nullptr);

// POST /jobs then poll GET until terminal or cancel. Never opens TSDB.
// GET /jobs/{id} and POST cancel send the owning clientId (X-Client-Id + JSON body).
ctrl::Message runJob(const Addr& addr, const JobSpec& spec, const std::atomic<bool>* cancel = nullptr);

ctrl::Message cancelJob(const Addr& addr, std::string_view jobId, std::string_view clientId);
bool cancelRemote(std::string_view addr, std::string_view jobId, std::string_view clientId);

// Temp bundle helper only. Refuses data/tsdb, data/video, and docker.sock paths.
void removeBundle(const std::string& path);

}  // namespace photon::analysis
