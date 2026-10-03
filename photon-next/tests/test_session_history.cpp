#include "photon/canp.hpp"
#include "photon/control.hpp"
#include "photon/dbc.hpp"
#include "photon/endian.hpp"
#include "photon/net.hpp"
#include "photon/protocol.hpp"
#include "photon/session.hpp"
#include "photon/tsdb.hpp"
#include "photon/websocket.hpp"
#include "photon/ws_handshake.hpp"
#include "photon_ui/tile_cache.hpp"

#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

void runSessionHistoryTests();

namespace {

namespace fs = std::filesystem;
namespace tsdb = photon::tsdb;

std::uint64_t wallMs() {
    using clock = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

fs::path uniqueDir(const char* tag) {
    const auto base = fs::temp_directory_path() / "photon-hist-e2e";
    fs::create_directories(base);
    static int n = 0;
    const auto dir = base / (std::string(tag) + "-" + std::to_string(++n));
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

bool sendAll(photon::net::Socket& sock, const std::uint8_t* data, std::size_t n) {
    std::size_t off = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (off < n) {
        const int w = sock.sendSome(data + off, n - off);
        if (w < 0) {
            return false;
        }
        if (w == 0) {
            if (std::chrono::steady_clock::now() > deadline) {
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
    const auto deadline = std::chrono::steady_clock::now() + wait;
    std::uint8_t tmp[4096];
    bool any = false;
    while (std::chrono::steady_clock::now() < deadline) {
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
                                         std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                                                       text.size()),
                                         mask);
}

bool parseServerFrame(std::vector<std::uint8_t>& buf, photon::ws::Opcode& op, std::vector<std::uint8_t>& payload) {
    if (buf.size() < 2) {
        return false;
    }
    const std::uint8_t b0 = buf[0];
    const std::uint8_t b1 = buf[1];
    op = static_cast<photon::ws::Opcode>(b0 & 0x0F);
    std::uint64_t len = b1 & 0x7F;
    std::size_t header = 2;
    if (len == 126) {
        if (buf.size() < 4) {
            return false;
        }
        std::size_t off = 2;
        std::uint16_t n16 = 0;
        photon::readU16Be(buf, off, n16);
        len = n16;
        header = 4;
    } else if (len == 127) {
        if (buf.size() < 10) {
            return false;
        }
        std::size_t off = 2;
        photon::readU64Be(buf, off, len);
        header = 10;
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        recvSomeInto(sock, buf, std::chrono::milliseconds(50));
        const std::string text(reinterpret_cast<const char*>(buf.data()), buf.size());
        if (text.find("\r\n\r\n") != std::string::npos && text.find("101") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void testHistoryDeliveryAndIngest() {
    photon::net::Guard sockets;
    const auto root = uniqueDir("e2e");
    const std::string sessionId = "e2e-hist";
    tsdb::SessionManifest man;
    man.id = sessionId;
    tsdb::Store w;
    std::string error;
    CHECK(w.create(root / sessionId, man, &error));
    tsdb::SignalInfo info;
    info.id = 1;
    info.name = "RPM";
    info.isSigned = false;
    info.factor = 0.25;
    w.setSignal(info);
    const std::vector<std::uint64_t> times = {1000, 1010, 1020, 1030, 1040};
    const std::vector<std::int64_t> raws = {400, 404, 408, 412, 416};
    for (std::size_t i = 0; i < times.size(); ++i) {
        w.append(1, times[i], raws[i]);
    }
    CHECK(w.finish(&error));
    w.close();

    photon::SessionHub hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {}, root.string());
    CHECK(hub.start());
    const auto port = hub.listenPort();
    CHECK(port != 0);

    std::atomic<bool> run{true};
    std::thread ticker([&] {
        while (run.load()) {
            hub.tick(wallMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    auto sock = photon::net::connectTcp("127.0.0.1", port);
    CHECK(sock.valid());
    CHECK(handshake(sock));

    const auto hello = photon::ctrl::serialize(photon::ctrl::hello("photon-tests"));
    {
        const auto frame = encodeClientText(hello);
        CHECK(sendAll(sock, frame.data(), frame.size()));
    }

    bool helloOk = false;
    std::vector<std::uint8_t> buf;
    {
        const auto helloDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < helloDeadline && !helloOk) {
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
                    helloOk = true;
                }
            }
        }
    }
    CHECK(helloOk);

    const auto query = photon::ctrl::serialize(
        photon::ctrl::historyQuery(42, sessionId, 1000, 1040, {1}, 2000, 4 * 1024 * 1024));
    {
        const auto frame = encodeClientText(query);
        CHECK(sendAll(sock, frame.data(), frame.size()));
    }

    bool sawResult = false;
    bool sawChunk = false;
    int resultLevel = -1;
    photon::ui::TileCache cache;
    cache.setSession(sessionId, "", 0, 10000);
    cache.bindQuery(42, 1, 1000, 1040, 5);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline && (!sawResult || !sawChunk)) {
        recvSomeInto(sock, buf, std::chrono::milliseconds(20));
        photon::ws::Opcode op{};
        std::vector<std::uint8_t> payload;
        while (parseServerFrame(buf, op, payload)) {
            if (op == photon::ws::Opcode::Text) {
                const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
                const auto msg = photon::ctrl::parse(text);
                if (msg && msg->type == photon::ctrl::Type::HistoryResult && msg->queryId == 42) {
                    sawResult = true;
                    resultLevel = msg->historyLevel;
                    cache.ingestResult(*msg);
                }
            } else if (op == photon::ws::Opcode::Binary && payload.size() >= photon::proto::kHeaderSize) {
                const auto decoded = photon::proto::decodeFrame(payload);
                if (decoded && decoded->header.msgType == 0x06) {
                    sawChunk = true;
                    const auto chunk = photon::proto::decodeHistoryChunk(decoded->payload);
                    CHECK(chunk.has_value());
                    if (chunk) {
                        CHECK(chunk->signalId == 1);
                        CHECK(chunk->level == 0);
                        CHECK(chunk->timestamps.size() == 5);
                        CHECK(chunk->timestamps[0] == 1000.0);
                        CHECK(chunk->timestamps[4] == 1040.0);
                        cache.ingestChunk(*chunk);
                    }
                }
            }
        }
    }

    CHECK(sawResult);
    CHECK(sawChunk);
    CHECK(resultLevel == 0);
    const auto plot = cache.gatherPlot(1, 1000, 1040, 0);
    CHECK(plot.t.size() == 5);
    CHECK(plot.y.size() == 5);
    CHECK(!plot.envelope);
    CHECK(plot.t.front() == 1000.0);
    CHECK(plot.t.back() == 1040.0);
    CHECK(plot.y.front() > 99.9 && plot.y.front() < 100.1);
    CHECK(plot.y.back() > 103.9 && plot.y.back() < 104.1);

    run.store(false);
    ticker.join();
    hub.stop();
}

bool helloClient(photon::net::Socket& sock, std::vector<std::uint8_t>& buf) {
    const auto hello = photon::ctrl::serialize(photon::ctrl::hello("photon-tests"));
    const auto frame = encodeClientText(hello);
    if (!sendAll(sock, frame.data(), frame.size())) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
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
                return true;
            }
        }
    }
    return false;
}

void testHistoryCancelAndShutdown() {
    photon::net::Guard sockets;
    const auto root = uniqueDir("cancel");
    const std::string sessionId = "e2e-cancel";
    tsdb::SessionManifest man;
    man.id = sessionId;
    tsdb::Store w;
    std::string error;
    CHECK(w.create(root / sessionId, man, &error));
    tsdb::SignalInfo info;
    info.id = 1;
    info.name = "RPM";
    info.isSigned = false;
    info.factor = 1.0;
    w.setSignal(info);
    for (int i = 0; i < 200; ++i) {
        w.append(1, static_cast<std::uint64_t>(i) * 10, i);
    }
    CHECK(w.finish(&error));
    w.close();

    {
        photon::SessionHub hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {},
                              root.string());
        CHECK(hub.start());
        std::atomic<bool> run{true};
        std::thread ticker([&] {
            while (run.load()) {
                hub.tick(wallMs());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        auto sock = photon::net::connectTcp("127.0.0.1", hub.listenPort());
        CHECK(sock.valid());
        CHECK(handshake(sock));
        std::vector<std::uint8_t> buf;
        CHECK(helloClient(sock, buf));
        const auto query = photon::ctrl::serialize(
            photon::ctrl::historyQuery(7, sessionId, 0, 2000, {1}, 2000, 4 * 1024 * 1024));
        CHECK(sendAll(sock, encodeClientText(query).data(), encodeClientText(query).size()));
        const auto cancel = photon::ctrl::serialize(photon::ctrl::historyCancel(7));
        CHECK(sendAll(sock, encodeClientText(cancel).data(), encodeClientText(cancel).size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        run.store(false);
        ticker.join();
        hub.stop();
    }

    {
        photon::SessionHub hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {},
                              root.string());
        CHECK(hub.start());
        std::atomic<bool> run{true};
        std::thread ticker([&] {
            while (run.load()) {
                hub.tick(wallMs());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        auto sock = photon::net::connectTcp("127.0.0.1", hub.listenPort());
        CHECK(sock.valid());
        CHECK(handshake(sock));
        std::vector<std::uint8_t> buf;
        CHECK(helloClient(sock, buf));
        const auto query = photon::ctrl::serialize(
            photon::ctrl::historyQuery(8, sessionId, 0, 2000, {1}, 2000, 4 * 1024 * 1024));
        const auto qframe = encodeClientText(query);
        CHECK(sendAll(sock, qframe.data(), qframe.size()));
        run.store(false);
        ticker.join();
        hub.stop();
    }
}

}  // namespace

bool waitForControl(photon::net::Socket& sock, std::vector<std::uint8_t>& buf, photon::ctrl::Type type,
                    photon::ctrl::Message& out, std::chrono::milliseconds wait) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < deadline) {
        recvSomeInto(sock, buf, std::chrono::milliseconds(20));
        photon::ws::Opcode op{};
        std::vector<std::uint8_t> payload;
        while (parseServerFrame(buf, op, payload)) {
            if (op != photon::ws::Opcode::Text) {
                continue;
            }
            const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
            const auto msg = photon::ctrl::parse(text);
            if (msg && msg->type == type) {
                out = *msg;
                return true;
            }
        }
    }
    return false;
}

void testVideoLocateWired() {
    photon::net::Guard sockets;
    photon::SessionHub hub(0, "127.0.0.1", 6500, {}, "127.0.0.1", 1, photon::ctrl::SourceKind::Synthetic, {}, {});
    CHECK(hub.start());
    std::atomic<bool> run{true};
    std::thread ticker([&] {
        while (run.load()) {
            hub.tick(wallMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    auto sock = photon::net::connectTcp("127.0.0.1", hub.listenPort());
    CHECK(sock.valid());
    CHECK(handshake(sock));

    const auto locate = photon::ctrl::serialize(photon::ctrl::videoLocate(1'000, 0, 4));
    CHECK(sendAll(sock, encodeClientText(locate).data(), encodeClientText(locate).size()));
    std::vector<std::uint8_t> buf;
    photon::ctrl::Message err;
    CHECK(waitForControl(sock, buf, photon::ctrl::Type::ErrorAlert, err, std::chrono::milliseconds(1500)));
    CHECK(err.code == "bad_message");

    CHECK(helloClient(sock, buf));
    CHECK(sendAll(sock, encodeClientText(locate).data(), encodeClientText(locate).size()));
    photon::ctrl::Message locErr;
    // The video connection deadline is 2500 ms, including on Windows where
    // a refused nonblocking connection can consume the entire poll timeout.
    CHECK(waitForControl(sock, buf, photon::ctrl::Type::ErrorAlert, locErr, std::chrono::milliseconds(4000)));
    CHECK(locErr.code == "video_locate");

    run.store(false);
    ticker.join();
    hub.stop();
}

void testHubLiveTailViaDaqTcp() {
    photon::net::Guard sockets;
    auto daq = photon::net::listenLoopback(0);
    CHECK(daq.valid());
    const auto daqPort = daq.localPort();
    CHECK(daqPort != 0);

    const auto root = uniqueDir("live-daq");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    photon::SessionHub hub(0, "127.0.0.1", daqPort, {}, {}, 0, photon::ctrl::SourceKind::DaqTcp, {},
                          root.string());
    CHECK(hub.start());

    std::atomic<bool> run{true};
    std::thread ticker([&] {
        while (run.load()) {
            hub.tick(wallMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    photon::net::Socket peer;
    const auto acceptDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!peer.valid() && std::chrono::steady_clock::now() < acceptDeadline) {
        photon::net::PollFd p{daq.native(), photon::net::kPollIn, 0};
        photon::net::poll(&p, 1, 20);
        if ((p.revents & photon::net::kPollIn) != 0) {
            peer = photon::net::acceptClient(daq);
        }
    }
    CHECK(peer.valid());

    std::string dbcError;
    auto db = photon::dbc::loadFile(std::string(PHOTON_ASSETS_DIR) + "/dbc/carcan_telemcan.dbc", &dbcError);
    CHECK(db.has_value());
    photon::dbc::EncodedFrame frame{};
    CHECK(photon::dbc::encodeFrame(*db, "BPS_Status",
                                   std::vector<photon::dbc::SignalValue>{{"Main_Battery_Voltage", 100.0}}, frame));

    const auto t0 = wallMs();
    for (int i = 0; i < 80; ++i) {
        photon::canp::Batch batch{};
        batch.seq = static_cast<std::uint32_t>(i + 1);
        batch.timestamp = t0 + static_cast<std::uint64_t>(i) * 50ull;
        batch.count = 1;
        batch.packets[0].canId = frame.canId;
        batch.packets[0].dlc = frame.dlc;
        std::memcpy(batch.packets[0].data, frame.data, 8);
        const auto wire = photon::canp::encodeBatch(batch);
        CHECK(sendAll(peer, wire.data(), wire.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    run.store(false);
    ticker.join();
    hub.stop();

    const auto sid = photon::dbc::signalIdOf("BPS_Status", "Main_Battery_Voltage");
    std::string liveId;
    bool found = false;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto sessionJson = entry.path() / "session.json";
        if (!fs::exists(sessionJson)) {
            continue;
        }
        tsdb::Store store;
        std::string error;
        CHECK(store.open(entry.path(), &error));
        tsdb::Query q;
        q.fromMs = t0;
        q.toMs = t0 + 10'000;
        q.signalIds = {sid};
        q.maxPoints = 2000;
        const auto result = store.query(q);
        CHECK(!result.series.empty());
        CHECK(!result.series[0].timestamps.empty());
        liveId = entry.path().filename().string();
        found = true;
        break;
    }
    CHECK(found);
    CHECK(!liveId.empty());

    photon::SessionHub reader(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {},
                              root.string());
    CHECK(reader.start());
    std::atomic<bool> readRun{true};
    std::thread readTicker([&] {
        while (readRun.load()) {
            reader.tick(wallMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    auto sock = photon::net::connectTcp("127.0.0.1", reader.listenPort());
    CHECK(sock.valid());
    CHECK(handshake(sock));
    std::vector<std::uint8_t> wsBuf;
    CHECK(helloClient(sock, wsBuf));
    const auto query = photon::ctrl::serialize(
        photon::ctrl::historyQuery(99, liveId, t0, t0 + 10'000, {sid}, 2000, 4 * 1024 * 1024));
    CHECK(sendAll(sock, encodeClientText(query).data(), encodeClientText(query).size()));
    bool sawLiveChunk = false;
    const auto histDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < histDeadline && !sawLiveChunk) {
        recvSomeInto(sock, wsBuf, std::chrono::milliseconds(20));
        photon::ws::Opcode op{};
        std::vector<std::uint8_t> payload;
        while (parseServerFrame(wsBuf, op, payload)) {
            if (op != photon::ws::Opcode::Binary || payload.size() < photon::proto::kHeaderSize) {
                continue;
            }
            const auto decoded = photon::proto::decodeFrame(payload);
            if (!decoded || decoded->header.msgType != 0x06) {
                continue;
            }
            const auto chunk = photon::proto::decodeHistoryChunk(decoded->payload);
            if (chunk && chunk->signalId == sid && !chunk->timestamps.empty()) {
                sawLiveChunk = true;
            }
        }
    }
    CHECK(sawLiveChunk);
    readRun.store(false);
    readTicker.join();
    reader.stop();
}

void testHubRecoversCrashedLive() {
    photon::net::Guard sockets;
    const auto root = uniqueDir("hub-recover");
    std::string error;
    {
        tsdb::Store writer;
        tsdb::LiveOptions opts;
        opts.pageMinSamples = 1;
        opts.checkpointMs = 100;
        tsdb::SessionManifest man;
        man.id = "hub-crash";
        CHECK(writer.createLive(root / "hub-crash", man, opts, &error));
        tsdb::SignalInfo info;
        info.id = 1;
        info.name = "RPM";
        writer.setSignal(info);
        writer.append(1, 1000, 4);
        CHECK(writer.checkpoint(1000, &error));
    }
    photon::SessionHub hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {},
                          root.string());
    CHECK(hub.start());
    hub.stop();
    tsdb::Store after;
    CHECK(after.open(root / "hub-crash", &error));
    CHECK(!after.manifest().live);
}

void runSessionHistoryTests() {
    testHistoryDeliveryAndIngest();
    testHistoryCancelAndShutdown();
    testVideoLocateWired();
    testHubLiveTailViaDaqTcp();
    testHubRecoversCrashedLive();
}
