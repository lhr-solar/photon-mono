#include "photon/analysis_compute.hpp"
#include "photon/cli.hpp"
#include "photon/control.hpp"
#include "photon/net.hpp"
#include "photon/session.hpp"
#include "photon/websocket.hpp"
#include "photon/ws_handshake.hpp"

#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

void runAnalysisComputeTests();

namespace {

using clock = std::chrono::steady_clock;

std::uint64_t wallMs() {
    using sys = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(sys::now().time_since_epoch()).count());
}

bool sendAll(photon::net::Socket& sock, const std::uint8_t* data, std::size_t n) {
    std::size_t off = 0;
    const auto deadline = clock::now() + std::chrono::seconds(2);
    while (off < n) {
        const int w = sock.sendSome(data + off, n - off);
        if (w < 0) {
            return false;
        }
        if (w == 0) {
            if (clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        off += static_cast<std::size_t>(w);
    }
    return true;
}

bool recvSomeInto(photon::net::Socket& sock, std::vector<std::uint8_t>& buf, std::chrono::milliseconds wait) {
    const auto deadline = clock::now() + wait;
    std::uint8_t tmp[4096];
    bool any = false;
    while (clock::now() < deadline) {
        const int n = sock.recvSome(tmp, sizeof(tmp));
        if (n < 0) {
            return any;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        buf.insert(buf.end(), tmp, tmp + n);
        any = true;
        if (n < static_cast<int>(sizeof(tmp))) {
            break;
        }
    }
    return any;
}

std::vector<std::uint8_t> encodeClientText(std::string_view text) {
    const std::uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    return photon::ws::encodeClientFrame(photon::ws::Opcode::Text,
                                         std::span<const std::uint8_t>(
                                             reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                                         mask);
}

bool parseServerFrame(std::vector<std::uint8_t>& buf, photon::ws::Opcode& op, std::vector<std::uint8_t>& payload) {
    if (buf.size() < 2) {
        return false;
    }
    const std::uint8_t b1 = buf[1];
    op = static_cast<photon::ws::Opcode>(buf[0] & 0x0F);
    std::uint64_t len = b1 & 0x7F;
    std::size_t header = 2;
    if (len == 126) {
        if (buf.size() < 4) {
            return false;
        }
        len = (static_cast<std::uint64_t>(buf[2]) << 8) | buf[3];
        header = 4;
    } else if (len == 127) {
        return false;
    }
    if (buf.size() < header + static_cast<std::size_t>(len)) {
        return false;
    }
    payload.assign(buf.begin() + static_cast<std::ptrdiff_t>(header),
                   buf.begin() + static_cast<std::ptrdiff_t>(header + len));
    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(header + len));
    return true;
}

bool handshake(photon::net::Socket& sock) {
    const char* req =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    if (!sendAll(sock, reinterpret_cast<const std::uint8_t*>(req), std::strlen(req))) {
        return false;
    }
    std::vector<std::uint8_t> buf;
    const auto deadline = clock::now() + std::chrono::seconds(2);
    while (clock::now() < deadline) {
        recvSomeInto(sock, buf, std::chrono::milliseconds(50));
        const std::string text(reinterpret_cast<const char*>(buf.data()), buf.size());
        if (text.find("\r\n\r\n") != std::string::npos && text.find("101") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool parseCli(std::vector<std::string> args, photon::cli::Options& opt) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& s : args) {
        argv.push_back(s.data());
    }
    return photon::cli::parseArgs(static_cast<int>(argv.size()), argv.data(), opt);
}

void testCliDefaultOff() {
    photon::cli::Options opt;
    CHECK(parseCli({"photon-backend"}, opt));
    CHECK(opt.analysisCompute.empty());
}

void testCliLoopback() {
    photon::cli::Options opt;
    CHECK(parseCli({"photon-backend", "--analysis-compute", "127.0.0.1:18765"}, opt));
    CHECK(opt.analysisCompute == "127.0.0.1:18765");

    photon::cli::Options eq;
    CHECK(parseCli({"photon-backend", "--analysis-compute=localhost:18765"}, eq));
    CHECK(eq.analysisCompute == "127.0.0.1:18765");

    photon::cli::Options off;
    CHECK(parseCli({"photon-backend", "--analysis-compute", "off"}, off));
    CHECK(off.analysisCompute.empty());
}

void testCliRejectsPublic() {
    photon::cli::Options lan;
    CHECK(!parseCli({"photon-backend", "--analysis-compute", "0.0.0.0:18765"}, lan));
    photon::cli::Options pub;
    CHECK(!parseCli({"photon-backend", "--analysis-compute", "192.168.1.9:18765"}, pub));
}

void testControlRoundTrip() {
    auto req = photon::ctrl::analysisComputeRequest("job-1", "sess-1", 1000, 2000, {1, 2}, "summarize_window", 500);
    req.dbcHash = "abc";
    req.prompt = "sum this";
    const auto reqBack = photon::ctrl::parse(photon::ctrl::serialize(req));
    CHECK(reqBack.has_value());
    CHECK(reqBack->type == photon::ctrl::Type::AnalysisComputeRequest);
    CHECK(reqBack->jobId == "job-1");
    CHECK(reqBack->sessionId == "sess-1");
    CHECK(reqBack->t0Ms == 1000);
    CHECK(reqBack->t1Ms == 2000);
    CHECK(reqBack->signalIds.size() == 2);
    CHECK(reqBack->recipeId == "summarize_window");
    CHECK(reqBack->dbcHash == "abc");
    CHECK(reqBack->maxPoints == 500);

    auto result = photon::ctrl::analysisComputeResult("job-1", "succeeded", "window ok", "", "");
    result.derived.push_back({"rpm_smooth", "rpm", "chunk:1"});
    result.events.push_back({1500, "over", "rpm high"});
    const auto resultBack = photon::ctrl::parse(photon::ctrl::serialize(result));
    CHECK(resultBack.has_value());
    CHECK(resultBack->type == photon::ctrl::Type::AnalysisComputeResult);
    CHECK(resultBack->computeStatus == "succeeded");
    CHECK(resultBack->summary == "window ok");
    CHECK(resultBack->derived.size() == 1);
    CHECK(resultBack->derived[0].name == "rpm_smooth");
    CHECK(resultBack->events.size() == 1);
    CHECK(resultBack->events[0].tMs == 1500);

    auto cancel = photon::ctrl::analysisComputeCancel("job-1");
    const auto cancelBack = photon::ctrl::parse(photon::ctrl::serialize(cancel));
    CHECK(cancelBack.has_value());
    CHECK(cancelBack->type == photon::ctrl::Type::AnalysisComputeCancel);
    CHECK(cancelBack->jobId == "job-1");
}

struct HubTicker {
    photon::SessionHub* hub = nullptr;
    std::atomic<bool> run{true};
    std::thread th;
    explicit HubTicker(photon::SessionHub& h) : hub(&h) {
        th = std::thread([this] {
            while (run.load()) {
                hub->tick(wallMs());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    ~HubTicker() {
        run.store(false);
        if (th.joinable()) {
            th.join();
        }
    }
};

bool helloOk(photon::net::Socket& sock) {
    if (!handshake(sock)) {
        return false;
    }
    const auto hello = photon::ctrl::serialize(photon::ctrl::hello("photon-tests"));
    const auto frame = encodeClientText(hello);
    if (!sendAll(sock, frame.data(), frame.size())) {
        return false;
    }
    std::vector<std::uint8_t> buf;
    const auto deadline = clock::now() + std::chrono::seconds(2);
    while (clock::now() < deadline) {
        recvSomeInto(sock, buf, std::chrono::milliseconds(20));
        photon::ws::Opcode op{};
        std::vector<std::uint8_t> payload;
        while (parseServerFrame(buf, op, payload)) {
            if (op != photon::ws::Opcode::Text) {
                continue;
            }
            const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
            const auto msg = photon::ctrl::parse(text);
            if (msg && msg->type == photon::ctrl::Type::HelloAck) {
                bool have = false;
                for (const auto& f : msg->features) {
                    if (f == "analysis_compute") {
                        have = true;
                    }
                }
                CHECK(have);
                return true;
            }
        }
    }
    return false;
}

std::optional<photon::ctrl::Message> waitComputeResult(photon::net::Socket& sock, const std::string& jobId,
                                                       std::chrono::milliseconds wait) {
    std::vector<std::uint8_t> buf;
    const auto deadline = clock::now() + wait;
    while (clock::now() < deadline) {
        recvSomeInto(sock, buf, std::chrono::milliseconds(20));
        photon::ws::Opcode op{};
        std::vector<std::uint8_t> payload;
        while (parseServerFrame(buf, op, payload)) {
            if (op != photon::ws::Opcode::Text) {
                continue;
            }
            const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
            const auto msg = photon::ctrl::parse(text);
            if (msg && msg->type == photon::ctrl::Type::AnalysisComputeResult && msg->jobId == jobId) {
                return msg;
            }
        }
    }
    return std::nullopt;
}

void testOfflineWhenFlagOff() {
    photon::net::Guard sockets;
    photon::SessionHub hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {}, {});
    CHECK(hub.start());
    HubTicker ticker(hub);
    auto sock = photon::net::connectTcp("127.0.0.1", hub.listenPort());
    CHECK(sock.valid());
    CHECK(helloOk(sock));
    const auto req = photon::ctrl::serialize(
        photon::ctrl::analysisComputeRequest("job-off", "sess", 0, 1000, {1}, "summarize_window"));
    const auto frame = encodeClientText(req);
    CHECK(sendAll(sock, frame.data(), frame.size()));
    const auto got = waitComputeResult(sock, "job-off", std::chrono::seconds(2));
    CHECK(got.has_value());
    CHECK(got->computeStatus == "failed");
    CHECK(got->code == photon::analysis::kOfflineCode);
}

void testOfflineWhenSidecarDown() {
    photon::net::Guard sockets;
    photon::SessionHub hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {}, {});
    hub.setAnalysisCompute("127.0.0.1:1");
    CHECK(hub.start());
    HubTicker ticker(hub);
    auto sock = photon::net::connectTcp("127.0.0.1", hub.listenPort());
    CHECK(sock.valid());
    CHECK(helloOk(sock));
    const auto req = photon::ctrl::serialize(
        photon::ctrl::analysisComputeRequest("job-down", "sess", 0, 1000, {1}, "summarize_window"));
    const auto frame = encodeClientText(req);
    CHECK(sendAll(sock, frame.data(), frame.size()));
    const auto got = waitComputeResult(sock, "job-down", std::chrono::seconds(3));
    CHECK(got.has_value());
    CHECK(got->computeStatus == "failed");
    CHECK(got->code == photon::analysis::kOfflineCode);
}

void mockSidecarLoop(photon::net::Socket& listener, std::atomic<bool>& run, std::string body) {
    const std::string header = "HTTP/1.1 202 Accepted\r\nContent-Type: application/json\r\nConnection: close\r\n"
                               "Content-Length: " +
                               std::to_string(body.size()) + "\r\n\r\n";
    const std::string resp = header + body;
    while (run.load()) {
        photon::net::PollFd pfd;
        pfd.fd = listener.native();
        pfd.events = photon::net::kPollIn;
        if (photon::net::poll(&pfd, 1, 40) <= 0) {
            continue;
        }
        auto client = photon::net::acceptClient(listener);
        if (!client.valid()) {
            continue;
        }
        std::vector<std::uint8_t> buf;
        recvSomeInto(client, buf, std::chrono::milliseconds(200));
        sendAll(client, reinterpret_cast<const std::uint8_t*>(resp.data()), resp.size());
    }
}

void testSidecarUpReturnsResult() {
    photon::net::Guard sockets;
    auto listener = photon::net::listenLoopback(0);
    CHECK(listener.valid());
    const auto port = listener.localPort();
    CHECK(port != 0);
    std::atomic<bool> run{true};
    const std::string body = R"({"jobId":"job-up","status":"succeeded","summary":"session sess window ok"})";
    std::thread mock([&] { mockSidecarLoop(listener, run, body); });

    photon::SessionHub hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {}, {});
    hub.setAnalysisCompute("127.0.0.1:" + std::to_string(static_cast<unsigned>(port)));
    CHECK(hub.start());
    HubTicker ticker(hub);
    auto sock = photon::net::connectTcp("127.0.0.1", hub.listenPort());
    CHECK(sock.valid());
    CHECK(helloOk(sock));
    const auto req = photon::ctrl::serialize(
        photon::ctrl::analysisComputeRequest("job-up", "sess", 1000, 2000, {1, 2}, "summarize_window"));
    const auto frame = encodeClientText(req);
    CHECK(sendAll(sock, frame.data(), frame.size()));
    const auto got = waitComputeResult(sock, "job-up", std::chrono::seconds(4));
    CHECK(got.has_value());
    CHECK(got->computeStatus == "succeeded");
    CHECK(got->summary.find("sess") != std::string::npos);

    run.store(false);
    if (mock.joinable()) {
        mock.join();
    }
}

void mockSidecarCountCancel(photon::net::Socket& listener, std::atomic<bool>& run, std::atomic<int>& cancels,
                            const std::string& body) {
    const std::string header = "HTTP/1.1 202 Accepted\r\nContent-Type: application/json\r\nConnection: close\r\n"
                               "Content-Length: " +
                               std::to_string(body.size()) + "\r\n\r\n";
    const std::string accepted = header + body;
    const std::string notFound =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found";
    while (run.load()) {
        photon::net::PollFd pfd;
        pfd.fd = listener.native();
        pfd.events = photon::net::kPollIn;
        if (photon::net::poll(&pfd, 1, 40) <= 0) {
            continue;
        }
        auto client = photon::net::acceptClient(listener);
        if (!client.valid()) {
            continue;
        }
        std::vector<std::uint8_t> buf;
        recvSomeInto(client, buf, std::chrono::milliseconds(200));
        const std::string req(reinterpret_cast<const char*>(buf.data()), buf.size());
        if (req.find("/cancel") != std::string::npos) {
            cancels.fetch_add(1);
            sendAll(client, reinterpret_cast<const std::uint8_t*>(notFound.data()), notFound.size());
            continue;
        }
        sendAll(client, reinterpret_cast<const std::uint8_t*>(accepted.data()), accepted.size());
    }
}

std::string httpResponse(int status, const char* reason, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

void mockSidecarHold(photon::net::Socket& listener, std::atomic<bool>& run, std::atomic<int>& cancels,
                     std::atomic<int>& posts, std::atomic<bool>& succeed, const std::string& runningBody,
                     const std::string& doneBody) {
    while (run.load()) {
        photon::net::PollFd pfd;
        pfd.fd = listener.native();
        pfd.events = photon::net::kPollIn;
        if (photon::net::poll(&pfd, 1, 40) <= 0) {
            continue;
        }
        auto client = photon::net::acceptClient(listener);
        if (!client.valid()) {
            continue;
        }
        std::vector<std::uint8_t> buf;
        recvSomeInto(client, buf, std::chrono::milliseconds(200));
        const std::string req(reinterpret_cast<const char*>(buf.data()), buf.size());
        std::string resp;
        if (req.find("/cancel") != std::string::npos) {
            cancels.fetch_add(1);
            resp = httpResponse(404, "Not Found", "not found");
        } else if (req.find("POST /jobs") != std::string::npos) {
            posts.fetch_add(1);
            resp = httpResponse(202, "Accepted", runningBody);
        } else if (succeed.load()) {
            resp = httpResponse(200, "OK", doneBody);
        } else {
            resp = httpResponse(200, "OK", runningBody);
        }
        sendAll(client, reinterpret_cast<const std::uint8_t*>(resp.data()), resp.size());
    }
}

void testUnknownCancelDoesNotHitSidecar() {
    photon::net::Guard sockets;
    auto listener = photon::net::listenLoopback(0);
    CHECK(listener.valid());
    const auto port = listener.localPort();
    CHECK(port != 0);
    std::atomic<bool> run{true};
    std::atomic<int> cancels{0};
    const std::string body = R"({"jobId":"shared-id","status":"running","summary":"secret-session-a"})";
    std::thread mock([&] { mockSidecarCountCancel(listener, run, cancels, body); });

    photon::SessionHub hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {}, {});
    hub.setAnalysisCompute("127.0.0.1:" + std::to_string(static_cast<unsigned>(port)));
    CHECK(hub.start());
    HubTicker ticker(hub);
    auto sock = photon::net::connectTcp("127.0.0.1", hub.listenPort());
    CHECK(sock.valid());
    CHECK(helloOk(sock));
    const auto cancel = photon::ctrl::serialize(photon::ctrl::analysisComputeCancel("shared-id"));
    const auto frame = encodeClientText(cancel);
    CHECK(sendAll(sock, frame.data(), frame.size()));
    const auto got = waitComputeResult(sock, "shared-id", std::chrono::seconds(2));
    CHECK(got.has_value());
    CHECK(got->computeStatus == "failed");
    CHECK(got->code == "unknown_job");
    CHECK(cancels.load() == 0);
    if (got->summary.find("secret-session-a") != std::string::npos ||
        got->message.find("secret-session-a") != std::string::npos) {
        CHECK(false);
    }

    run.store(false);
    if (mock.joinable()) {
        mock.join();
    }
}

void testCrossClientCancelDoesNotForward() {
    photon::net::Guard sockets;
    auto listener = photon::net::listenLoopback(0);
    CHECK(listener.valid());
    const auto port = listener.localPort();
    CHECK(port != 0);
    std::atomic<bool> run{true};
    std::atomic<int> cancels{0};
    std::atomic<int> posts{0};
    std::atomic<bool> succeed{false};
    const std::string running = R"({"jobId":"job-a","status":"running","summary":"secret-session-a"})";
    const std::string done = R"({"jobId":"job-a","status":"succeeded","summary":"secret-session-a"})";
    std::thread mock([&] { mockSidecarHold(listener, run, cancels, posts, succeed, running, done); });

    photon::SessionHub hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {}, {});
    hub.setAnalysisCompute("127.0.0.1:" + std::to_string(static_cast<unsigned>(port)));
    CHECK(hub.start());
    HubTicker ticker(hub);

    auto sockA = photon::net::connectTcp("127.0.0.1", hub.listenPort());
    CHECK(sockA.valid());
    CHECK(helloOk(sockA));
    auto sockB = photon::net::connectTcp("127.0.0.1", hub.listenPort());
    CHECK(sockB.valid());
    CHECK(helloOk(sockB));

    const auto req = photon::ctrl::serialize(
        photon::ctrl::analysisComputeRequest("job-a", "secret-session-a", 1000, 2000, {1}, "summarize_window"));
    const auto frameA = encodeClientText(req);
    CHECK(sendAll(sockA, frameA.data(), frameA.size()));

    const auto postDeadline = clock::now() + std::chrono::seconds(2);
    while (posts.load() == 0 && clock::now() < postDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(posts.load() >= 1);

    const auto cancel = photon::ctrl::serialize(photon::ctrl::analysisComputeCancel("job-a"));
    const auto frameB = encodeClientText(cancel);
    CHECK(sendAll(sockB, frameB.data(), frameB.size()));
    const auto gotB = waitComputeResult(sockB, "job-a", std::chrono::seconds(2));
    CHECK(gotB.has_value());
    CHECK(gotB->computeStatus == "failed");
    CHECK(gotB->code == "unknown_job");
    CHECK(cancels.load() == 0);
    CHECK(gotB->summary.find("secret-session-a") == std::string::npos);
    CHECK(gotB->message.find("secret-session-a") == std::string::npos);

    succeed.store(true);
    const auto gotA = waitComputeResult(sockA, "job-a", std::chrono::seconds(4));
    CHECK(gotA.has_value());
    CHECK(gotA->computeStatus == "succeeded");
    CHECK(gotA->summary.find("secret-session-a") != std::string::npos);

    run.store(false);
    if (mock.joinable()) {
        mock.join();
    }
}

void testCancelEmptyClientIdSkipsSidecar() {
    photon::analysis::Addr addr;
    addr.host = "127.0.0.1";
    addr.port = 1;
    const auto msg = photon::analysis::cancelJob(addr, "job-x", "");
    CHECK(msg.computeStatus == "failed");
    CHECK(msg.code == std::string("unknown_job"));
    CHECK(msg.message.find("secret-session-a") == std::string::npos);
}

void testParseAddrLoopbackOnly() {
    photon::analysis::Addr addr;
    CHECK(!photon::analysis::parseAddr("", addr));
    CHECK(!photon::analysis::parseAddr("off", addr));
    CHECK(!photon::analysis::parseAddr("0.0.0.0:18765", addr));
    CHECK(!photon::analysis::parseAddr("10.0.0.5:18765", addr));
    CHECK(photon::analysis::parseAddr("127.0.0.1:18765", addr));
    CHECK(addr.host == "127.0.0.1");
    CHECK(addr.port == 18765);
}

}  // namespace

void runAnalysisComputeTests() {
    testCliDefaultOff();
    testCliLoopback();
    testCliRejectsPublic();
    testControlRoundTrip();
    testParseAddrLoopbackOnly();
    testOfflineWhenFlagOff();
    testOfflineWhenSidecarDown();
    testSidecarUpReturnsResult();
    testUnknownCancelDoesNotHitSidecar();
    testCrossClientCancelDoesNotForward();
    testCancelEmptyClientIdSkipsSidecar();
}
