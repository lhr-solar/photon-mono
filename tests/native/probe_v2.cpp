// Protocol v2 end-to-end probe: drives the real photon_ui::Client against a running
// photon-server and reports what the arena received. A console tool, not a ctest test.
//
//   photon-v2-probe <token> <seconds>                     subscribe to BPS_Status.Main_Battery_Voltage
//   photon-v2-probe <token> <seconds> --expect-refused    wrong token: expect the 1008 refusal, then no retry
//
// Exit 0 on PASS, 1 on FAIL, 2 on bad arguments.

#include "photon/net.hpp"
#include "photon_ui/arena.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/platform.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <thread>

namespace {

using photon::ui::Client;
using photon::ui::ClientArena;

constexpr const char* kSignalName = "BPS_Status.Main_Battery_Voltage";
constexpr double kExpectedV = 350.0;
constexpr double kCursorSlackMs = 1000.0;
constexpr double kQuietMs = 3000.0;

double wallMs() {
    using namespace std::chrono;
    return static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void printHello(const ClientArena& arena) {
    std::printf("hello_ack: helloOk=%d server=%s user=%s liveRun=%s clockOffsetNs=%lld levels=[",
                arena.server.helloOk ? 1 : 0, arena.server.server.c_str(), arena.server.user.c_str(),
                arena.server.liveRun.c_str(), static_cast<long long>(arena.server.clockOffsetNs));
    for (std::size_t i = 0; i < arena.server.levels.size(); ++i) {
        std::printf("%s%lld", i ? "," : "", static_cast<long long>(arena.server.levels[i]));
    }
    std::printf("]\nfeatures:");
    for (const auto& f : arena.features) {
        std::printf(" %s", f.c_str());
    }
    std::printf("\n");
}

void printLogTail(const ClientArena& arena, std::size_t lines) {
    const auto& logs = arena.logs();
    const std::size_t from = logs.size() > lines ? logs.size() - lines : 0;
    std::printf("log (last %zu of %zu):\n", logs.size() - from, logs.size());
    for (std::size_t i = from; i < logs.size(); ++i) {
        std::printf("  %s%s\n", logs[i].error ? "! " : "  ", logs[i].text.c_str());
    }
}

int runTest(ClientArena& arena, Client& client, double seconds) {
    const double start = photon::ui::nowMs();
    std::uint32_t id = 0;
    bool subscribed = false;
    bool signalMissing = false;
    double subscribedAt = 0.0;
    double firstSampleAt = -1.0;
    while (photon::ui::nowMs() - start < seconds * 1000.0) {
        client.tick(photon::ui::nowMs());
        if (!subscribed && arena.haveSignalCatalog) {
            for (const auto& sig : arena.signalCatalog) {
                if (sig.message + "." + sig.name == kSignalName) {
                    id = sig.id;
                    break;
                }
            }
            if (id == 0) {
                signalMissing = true;
                break;
            }
            client.ensureSubscribed(std::span<const std::uint32_t>(&id, 1));
            subscribed = true;
            subscribedAt = photon::ui::nowMs();
        }
        if (subscribed && firstSampleAt < 0.0) {
            const auto* ring = arena.ring(id);
            if (ring != nullptr && ring->count > 0) {
                firstSampleAt = photon::ui::nowMs();
            }
        }
        sleepMs(1);
    }

    std::printf("probe: url=%s state=%s seconds=%.1f\n", client.url().c_str(), client.stateLabel(), seconds);
    printHello(arena);

    std::printf("catalog: arrived=%d sha=%.12s signals=%zu buses=%zu first:",
                arena.haveSignalCatalog ? 1 : 0, arena.catalogSha.c_str(), arena.signalCatalog.size(),
                arena.catalogBuses.size());
    for (std::size_t i = 0; i < 3 && i < arena.signalCatalog.size(); ++i) {
        const auto& sig = arena.signalCatalog[i];
        std::printf(" [%u]%s.%s", static_cast<unsigned>(sig.id), sig.message.c_str(), sig.name.c_str());
    }
    std::printf("\n");

    if (signalMissing) {
        std::printf("subscribe: %s is not in the catalog\n", kSignalName);
        std::printf("RESULT: FAIL\n");
        printLogTail(arena, 25);
        return 1;
    }
    if (!subscribed) {
        std::printf("subscribe: no catalog within %.1f s\n", seconds);
        std::printf("RESULT: FAIL\n");
        printLogTail(arena, 25);
        return 1;
    }

    const auto* ring = arena.ring(id);
    const bool haveSamples = ring != nullptr && ring->count > 0;
    std::string unit;
    for (const auto& sig : arena.signalCatalog) {
        if (sig.id == id) {
            unit = sig.unit;
            break;
        }
    }
    const std::string firstSample =
        firstSampleAt < 0.0 ? std::string("none")
                            : std::to_string(static_cast<long long>(firstSampleAt - subscribedAt));
    std::printf("subscribe: name=%s id=%u index=%u unit=%s subscribed=%d first_sample_after_ms=%s\n",
                kSignalName, static_cast<unsigned>(id), static_cast<unsigned>(id - 1), unit.c_str(),
                client.subscribed().contains(id) ? 1 : 0, firstSample.c_str());
    if (haveSamples) {
        std::printf("ring: count=%zu write=%zu hasLatest=%d latest=%.9g latestTs=%.3f firstTs=%.3f lastTs=%.3f\n",
                    ring->count, ring->write, ring->hasLatest ? 1 : 0, ring->latest, ring->latestTs,
                    ring->firstTs(), ring->lastTs());
    } else {
        std::printf("ring: count=0 (no samples for %s)\n", kSignalName);
    }

    const double now = wallMs();
    const double cursor = static_cast<double>(arena.timeline().cursorMs);
    const bool cursorOk = std::fabs(now - cursor) <= kCursorSlackMs;
    std::printf("cursor: live=%d cursorMs=%.0f wall_ms=%.0f diff_ms=%.0f\n", arena.timelineIsLive() ? 1 : 0,
                cursor, now, now - cursor);
    std::printf("gaps: %zu\n", arena.gaps().size());

    const bool latestOk = haveSamples && ring->hasLatest && std::fabs(ring->latest - kExpectedV) < 1e-6;
    const bool ok = haveSamples && latestOk && cursorOk;
    std::printf("checks: samples=%d latest_350=%d cursor_within_1s=%d\n", haveSamples ? 1 : 0, latestOk ? 1 : 0,
                cursorOk ? 1 : 0);
    printLogTail(arena, 25);
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int runRefused(ClientArena& arena, Client& client, double seconds) {
    const double start = photon::ui::nowMs();
    double refusedAt = -1.0;
    std::size_t logsAtRefusal = 0;
    std::string lastAtRefusal;
    bool reconnected = false;
    while (true) {
        const double now = photon::ui::nowMs();
        client.tick(now);
        if (refusedAt < 0.0) {
            if (client.loginRefused()) {
                refusedAt = now;
                logsAtRefusal = arena.logs().size();
                lastAtRefusal = arena.logs().empty() ? std::string() : arena.logs().back().text;
            } else if (now - start >= seconds * 1000.0) {
                break;
            }
        } else {
            const bool logMoved = arena.logs().size() != logsAtRefusal ||
                                  (!arena.logs().empty() && arena.logs().back().text != lastAtRefusal);
            if (client.state() != Client::State::Disconnected || logMoved) {
                reconnected = true;
                break;
            }
            if (now - refusedAt >= kQuietMs) {
                break;
            }
        }
        sleepMs(1);
    }

    std::printf("probe: url=%s mode=expect-refused seconds=%.1f\n", client.url().c_str(), seconds);
    if (refusedAt < 0.0) {
        std::printf("refusal: none within %.1f s (state=%s)\n", seconds, client.stateLabel());
        std::printf("RESULT: FAIL\n");
        printLogTail(arena, 25);
        return 1;
    }
    std::string reason = "(no 'bad token' close line in the log)";
    for (const auto& line : arena.logs()) {
        if (line.text.find("bad token") != std::string::npos) {
            reason = line.text;
        }
    }
    std::printf("refusal: loginRefused=%d state=%d stateLabel=%s helloOk=%d\n", client.loginRefused() ? 1 : 0,
                static_cast<int>(client.state()), client.stateLabel(), arena.server.helloOk ? 1 : 0);
    std::printf("reason: %s\n", reason.c_str());
    std::printf("quiet: reconnected=%d window_ms=%.0f\n", reconnected ? 1 : 0, kQuietMs);
    const bool ok = client.loginRefused() && client.state() == Client::State::Disconnected &&
                    !arena.server.helloOk && !reconnected && reason.find("bad token") != std::string::npos;
    printLogTail(arena, 25);
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: photon-v2-probe <token> <seconds> [--expect-refused]\n");
        return 2;
    }
    const std::string token = argv[1];
    const double seconds = std::atof(argv[2]);
    const bool expectRefused = argc > 3 && std::strcmp(argv[3], "--expect-refused") == 0;
    if (!(seconds > 0.0)) {
        std::fprintf(stderr, "seconds must be positive\n");
        return 2;
    }

    // Winsock starts in Guard's constructor; without it every socket call fails (as in ui/src/main.cpp).
    photon::net::Guard sockets;
    ClientArena arena;
    Client client(arena);
    client.setToken(token);
    client.setUrl("ws://127.0.0.1:6900/v2");
    const int rc = expectRefused ? runRefused(arena, client, seconds) : runTest(arena, client, seconds);
    std::fflush(stdout);
    return rc;
}
