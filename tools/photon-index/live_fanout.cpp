#include "live_fanout.hpp"

#include "photon/canp.hpp"
#include "photon/client_auth.hpp"
#include "photon/dbc.hpp"
#include "photon/tsdb.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
static constexpr Socket kInvalid = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
static constexpr Socket kInvalid = -1;
#endif

namespace fs = std::filesystem;

namespace {

bool stopped(const LiveFanoutOptions& opt) {
    return opt.stop != nullptr && opt.stop->load();
}

void closeSock(Socket s) {
    if (s == kInvalid) {
        return;
    }
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

struct WinsockGuard {
#if defined(_WIN32)
    bool ok = false;
    WinsockGuard() {
        WSADATA wsa{};
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    ~WinsockGuard() {
        if (ok) {
            WSACleanup();
        }
    }
#else
    bool ok = true;
#endif
};

Socket connectFanout(const std::string& host, std::uint16_t port) {
    Socket sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalid) {
        return kInvalid;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        closeSock(sock);
        return kInvalid;
    }
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSock(sock);
        return kInvalid;
    }
    return sock;
}

void writeLivePointer(const fs::path& root, const std::string& sessionId, bool live) {
    std::error_code ec;
    fs::create_directories(root, ec);
    std::ofstream out(root / "live.json", std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }
    out << "{\"session_id\":\"" << sessionId << "\",\"live\":" << (live ? "true" : "false") << "}\n";
}

void publishVideoVpt(const fs::path& sessionDir, const std::string& videoVptPath, std::uint64_t minMs,
                     std::uint64_t maxMs) {
    if (videoVptPath.empty() || !fs::exists(videoVptPath)) {
        return;
    }
    photon::tsdb::copySessionVideoVpt(videoVptPath, sessionDir / "video.vpt", minMs, maxMs);
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

int runLiveFanout(const LiveFanoutOptions& opt) {
    if (opt.outDir.empty() || opt.dbcPath.empty()) {
        std::fprintf(stderr, "[live-fanout] --out and --dbc are required\n");
        return 2;
    }

    WinsockGuard ws;
    if (!ws.ok) {
        std::fprintf(stderr, "[live-fanout] winsock init failed\n");
        return 1;
    }

    std::string error;
    auto db = photon::dbc::loadFile(opt.dbcPath, &error);
    if (!db) {
        std::fprintf(stderr, "[live-fanout] DBC load failed: %s\n", error.c_str());
        return 1;
    }

    const fs::path root(opt.outDir);
    std::error_code ec;
    fs::create_directories(root, ec);
    std::string recoverError;
    photon::tsdb::recoverCrashedSessions(root, &recoverError);

    std::unique_ptr<photon::tsdb::LiveTailWriter> writer;
    std::string sessionId;
    std::uint64_t lastMs = 0;
    std::uint64_t frames = 0;
    photon::canp::StreamParser parser;

    auto seal = [&]() {
        if (!writer || sessionId.empty()) {
            writer.reset();
            sessionId.clear();
            lastMs = 0;
            return;
        }
        std::string endError;
        const auto& man = writer->store().manifest();
        photon::tsdb::upsertCatalog(root, sessionId, man.minMs, man.maxMs, man.dbcSha, nullptr);
        publishVideoVpt(root / sessionId, opt.videoVptPath, man.minMs, man.maxMs);
        if (!writer->end(&endError) && !endError.empty()) {
            std::fprintf(stderr, "[live-fanout] end: %s\n", endError.c_str());
        }
        writeLivePointer(root, sessionId, false);
        writer.reset();
        sessionId.clear();
        lastMs = 0;
    };

    auto openSession = [&](std::uint64_t firstMs) -> bool {
        int seq = 0;
        std::string id;
        for (;;) {
            id = photon::tsdb::sessionIdFromMs(firstMs, seq);
            if (!fs::exists(root / id, ec)) {
                break;
            }
            ++seq;
            if (seq > 99) {
                return false;
            }
        }
        auto next = std::make_unique<photon::tsdb::LiveTailWriter>();
        photon::tsdb::LiveOptions opts;
        std::string beginError;
        if (!next->begin(root, id, *db, opts, &beginError)) {
            std::fprintf(stderr, "[live-fanout] begin failed: %s\n", beginError.c_str());
            return false;
        }
        writer = std::move(next);
        sessionId = std::move(id);
        lastMs = firstMs;
        writeLivePointer(root, sessionId, true);
        publishVideoVpt(root / sessionId, opt.videoVptPath, firstMs, firstMs);
        std::fprintf(stdout, "[live-fanout] session %s\n", sessionId.c_str());
        return true;
    };

    std::fprintf(stdout, "[live-fanout] %s:%u -> %s\n", opt.host.c_str(), static_cast<unsigned>(opt.port),
                 opt.outDir.c_str());

    while (!stopped(opt) && (opt.maxFrames == 0 || frames < opt.maxFrames)) {
        Socket sock = connectFanout(opt.host, opt.port);
        if (sock == kInvalid) {
            sleepMs(200);
            continue;
        }
        std::fprintf(stdout, "[live-fanout] connected %s:%u\n", opt.host.c_str(), static_cast<unsigned>(opt.port));
        if (!opt.password.empty()) {
            std::vector<std::uint8_t> auth;
            if (!photon::auth::encode(opt.password, auth)) {
                closeSock(sock);
                std::fprintf(stderr, "[live-fanout] invalid password\n");
                return 2;
            }
#if defined(_WIN32)
            const int sent = send(sock, reinterpret_cast<const char*>(auth.data()), static_cast<int>(auth.size()), 0);
#else
            const ssize_t sent = send(sock, auth.data(), auth.size(), 0);
#endif
            if (sent != static_cast<int>(auth.size())) {
                closeSock(sock);
                sleepMs(200);
                continue;
            }
        }
        parser.clear();
        std::uint8_t buf[32 * 1024];
        bool connected = true;
        while (connected && !stopped(opt) && (opt.maxFrames == 0 || frames < opt.maxFrames)) {
#if defined(_WIN32)
            const int n = recv(sock, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t n = recv(sock, buf, sizeof(buf), 0);
#endif
            if (n <= 0) {
                connected = false;
                break;
            }
            parser.feed(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
            while (auto batch = parser.take()) {
                for (std::uint16_t i = 0; i < batch->count; ++i) {
                    const auto& pkt = batch->packets[i];
                    const std::uint64_t t = batch->timestamp;
                    if (!sessionId.empty() && photon::tsdb::isSessionBoundary(t, lastMs)) {
                        seal();
                    }
                    if (sessionId.empty() && !openSession(t)) {
                        closeSock(sock);
                        return 1;
                    }
                    writer->onFrame(pkt.canId, pkt.dlc, pkt.data, t);
                    if (t >= lastMs) {
                        lastMs = t;
                    }
                    ++frames;
                    if (opt.maxFrames != 0 && frames >= opt.maxFrames) {
                        break;
                    }
                }
                if (writer && !sessionId.empty()) {
                    const auto& man = writer->store().manifest();
                    publishVideoVpt(root / sessionId, opt.videoVptPath, man.minMs, man.maxMs);
                    photon::tsdb::upsertCatalog(root, sessionId, man.minMs, man.maxMs, man.dbcSha, nullptr);
                }
                if (opt.maxFrames != 0 && frames >= opt.maxFrames) {
                    break;
                }
            }
        }
        closeSock(sock);
        if (opt.maxFrames != 0 && frames >= opt.maxFrames) {
            break;
        }
        if (!stopped(opt)) {
            std::fprintf(stdout, "[live-fanout] disconnected; retrying\n");
            sleepMs(200);
        }
    }

    seal();
    std::fprintf(stdout, "[live-fanout] frames=%llu\n", static_cast<unsigned long long>(frames));
    return 0;
}
