#include "photon/canp.hpp"
#include "photon/control.hpp"
#include "photon/endian.hpp"
#include "photon/net.hpp"
#include "photon/protocol.hpp"
#include "photon/session.hpp"
#include "photon/source.hpp"
#include "photon/vsd1.hpp"
#include "photon/websocket.hpp"
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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

void runLogSwitchTests();

namespace {

namespace fs = std::filesystem;

std::vector<std::uint8_t> encodeVsd1(std::uint64_t id, const photon::canp::Batch& batch) {
    const auto payload = photon::canp::encodeBatch(batch);
    std::vector<std::uint8_t> out;
    photon::writeU32Be(out, photon::vsd1::kMagic);
    photon::writeU32Be(out, static_cast<std::uint32_t>(payload.size()));
    photon::writeU64Be(out, id);
    photon::writeU32Be(out, 0);
    photon::writeU32Be(out, 0);
    photon::writeBytes(out, payload);
    return out;
}

photon::canp::Batch makeBatch(std::uint32_t seq, std::uint64_t ts, std::uint32_t canId, std::uint8_t first) {
    photon::canp::Batch batch{};
    batch.seq = seq;
    batch.timestamp = ts;
    batch.count = 1;
    batch.packets[0].canId = canId;
    batch.packets[0].dlc = 8;
    batch.packets[0].data[0] = first;
    return batch;
}

void writeLog(const fs::path& file, const std::vector<std::uint64_t>& times, std::uint32_t canId = 0x100) {
    fs::create_directories(file.parent_path());
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < times.size(); ++i) {
        auto frame = encodeVsd1(i + 1, makeBatch(static_cast<std::uint32_t>(i + 1), times[i], canId,
                                                 static_cast<std::uint8_t>(i + 1)));
        bytes.insert(bytes.end(), frame.begin(), frame.end());
    }
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

fs::path uniqueRoot(const char* tag) {
    static int n = 0;
    const auto dir = fs::temp_directory_path() / "photon-log-switch" / (std::string(tag) + "-" + std::to_string(++n));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool waitBound(photon::SourceHub& hub, photon::ReplayView& view, std::uint64_t& now) {
    for (int i = 0; i < 400; ++i) {
        hub.tick(now);
        hub.attachReplay(view);
        if (view.bound) {
            return true;
        }
        now += 20;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
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
        std::uint64_t n64 = 0;
        photon::readU64Be(buf, off, n64);
        len = n64;
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

bool sendJson(photon::net::Socket& sock, const photon::ctrl::Message& msg) {
    const auto json = photon::ctrl::serialize(msg);
    const auto frame = encodeClientText(json);
    return sendAll(sock, frame.data(), frame.size());
}

struct WsInbox {
    std::vector<photon::ctrl::Message> text;
    std::vector<photon::proto::TimelineStatus> timelines;
    bool error = false;
    std::string errorCode;
};

void drain(photon::net::Socket& sock, std::vector<std::uint8_t>& buf, WsInbox& inbox,
           std::chrono::milliseconds wait) {
    recvSomeInto(sock, buf, wait);
    photon::ws::Opcode op{};
    std::vector<std::uint8_t> payload;
    while (parseServerFrame(buf, op, payload)) {
        if (op == photon::ws::Opcode::Text) {
            const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
            const auto msg = photon::ctrl::parse(text);
            if (!msg) {
                continue;
            }
            if (msg->type == photon::ctrl::Type::ErrorAlert) {
                inbox.error = true;
                inbox.errorCode = msg->code;
            }
            inbox.text.push_back(*msg);
        } else if (op == photon::ws::Opcode::Binary && payload.size() >= photon::proto::kHeaderSize) {
            const auto decoded = photon::proto::decodeFrame(payload);
            if (decoded && decoded->header.msgType == static_cast<std::uint8_t>(photon::proto::MsgType::TimelineStatus)) {
                const auto st = photon::proto::decodeTimelineStatus(decoded->payload);
                if (st) {
                    inbox.timelines.push_back(*st);
                }
            }
        }
    }
}

const photon::ctrl::Message* lastOf(const WsInbox& inbox, photon::ctrl::Type type) {
    for (auto it = inbox.text.rbegin(); it != inbox.text.rend(); ++it) {
        if (it->type == type) {
            return &*it;
        }
    }
    return nullptr;
}

bool helloClient(photon::net::Socket& sock, std::vector<std::uint8_t>& buf, WsInbox& inbox) {
    if (!sendJson(sock, photon::ctrl::hello("photon-tests"))) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        drain(sock, buf, inbox, std::chrono::milliseconds(20));
        if (lastOf(inbox, photon::ctrl::Type::HelloAck) != nullptr) {
            return true;
        }
    }
    return false;
}

bool waitAckPath(photon::net::Socket& sock, std::vector<std::uint8_t>& buf, WsInbox& inbox,
                 const std::string& needle) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        drain(sock, buf, inbox, std::chrono::milliseconds(20));
        if (const auto* ack = lastOf(inbox, photon::ctrl::Type::HelloAck)) {
            if (ack->activeSource.path.find(needle) != std::string::npos) {
                return true;
            }
        }
        if (inbox.error) {
            return false;
        }
    }
    return false;
}

void testSourceHubSwitchToOlderLog() {
    const auto root = uniqueRoot("hub-old");
    const auto newer = root / "newer";
    const auto older = root / "older";
    writeLog(newer / "run.log", {5000, 6000, 7000});
    writeLog(older / "run.log", {100, 200, 300});

    photon::SourceHub hub("127.0.0.1", 6500);
    std::string error;
    CHECK(hub.select(photon::ctrl::SourceKind::DaqTcp, "127.0.0.1", 6500, "", error));
    CHECK(hub.openLog(newer.string(), error));
    photon::ReplayView view;
    std::uint64_t now = 1;
    CHECK(waitBound(hub, view, now));
    auto tl = hub.replayTimeline(view);
    CHECK(tl.minTimeMs == 5000);
    CHECK(tl.maxTimeMs == 7000);

    hub.resetReplay(view);
    CHECK(hub.openLog(older.string(), error));
    CHECK(hub.logPath().find("older") != std::string::npos);
    CHECK(waitBound(hub, view, now));
    tl = hub.replayTimeline(view);
    CHECK(tl.minTimeMs == 100);
    CHECK(tl.maxTimeMs == 300);
    CHECK(tl.cursorMs >= 100 && tl.cursorMs <= 300);

    hub.controlReplay(view, photon::ctrl::TimelineMode::Pause, 7000, 1.0);
    tl = hub.replayTimeline(view);
    CHECK(tl.cursorMs == 300);

    hub.resetReplay(view);
    CHECK(hub.openLog(newer.string(), error));
    CHECK(waitBound(hub, view, now));
    tl = hub.replayTimeline(view);
    CHECK(tl.minTimeMs == 5000);
    CHECK(tl.maxTimeMs == 7000);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSourceHubBadPathKeepsCurrentLog() {
    const auto root = uniqueRoot("hub-bad");
    writeLog(root / "keep" / "run.log", {1000, 1100, 1200});
    // A directory whose name ends in .zip still has .log files, so resolveReplayPath
    // will not remap onto the machine catalog — open() then rejects the zip suffix.
    writeLog(root / "nolog.zip" / "run.log", {1, 2});
    photon::SourceHub hub("127.0.0.1", 6500);
    std::string error;
    CHECK(hub.select(photon::ctrl::SourceKind::Synthetic, "", 0, "", error));
    CHECK(hub.openLog((root / "keep").string(), error));
    photon::ReplayView view;
    std::uint64_t now = 1;
    CHECK(waitBound(hub, view, now));
    const auto before = hub.logPath();

    std::string fail;
    CHECK(!hub.openLog((root / "nolog.zip").string(), fail));
    CHECK(!fail.empty());
    CHECK(hub.logPath() == before);
    hub.tickReplay(view, now + 50);
    const auto tl = hub.replayTimeline(view);
    CHECK(tl.minTimeMs == 1000);
    CHECK(tl.maxTimeMs == 1200);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSourceHubSamePathIsNoop() {
    const auto root = uniqueRoot("hub-same");
    writeLog(root / "day" / "run.log", {10, 20, 30});
    photon::SourceHub hub("127.0.0.1", 6500);
    std::string error;
    CHECK(hub.select(photon::ctrl::SourceKind::DaqTcp, "127.0.0.1", 1, "", error));
    CHECK(hub.openLog((root / "day").string(), error));
    photon::ReplayView view;
    std::uint64_t now = 1;
    CHECK(waitBound(hub, view, now));
    hub.controlReplay(view, photon::ctrl::TimelineMode::Pause, 20, 1.0);
    CHECK(hub.openLog((root / "day").string(), error));
    CHECK(view.bound);
    CHECK(hub.replayTimeline(view).cursorMs == 20);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSourceHubTwoViewsSurviveSwitch() {
    const auto root = uniqueRoot("hub-two");
    writeLog(root / "a" / "run.log", {1000, 2000});
    writeLog(root / "b" / "run.log", {50, 60, 70});
    photon::SourceHub hub("127.0.0.1", 6500);
    std::string error;
    CHECK(hub.select(photon::ctrl::SourceKind::DaqTcp, "127.0.0.1", 6500, "", error));
    CHECK(hub.openLog((root / "a").string(), error));
    photon::ReplayView alice;
    photon::ReplayView bob;
    std::uint64_t now = 1;
    CHECK(waitBound(hub, alice, now));
    CHECK(waitBound(hub, bob, now));
    hub.controlReplay(alice, photon::ctrl::TimelineMode::Pause, 1000, 1.0);
    hub.controlReplay(bob, photon::ctrl::TimelineMode::Pause, 2000, 1.0);

    hub.resetReplay(alice);
    hub.resetReplay(bob);
    CHECK(hub.openLog((root / "b").string(), error));
    CHECK(waitBound(hub, alice, now));
    CHECK(waitBound(hub, bob, now));
    for (int i = 0; i < 40; ++i) {
        hub.tickReplay(alice, now);
        hub.tickReplay(bob, now);
        now += 16;
    }
    CHECK(hub.replayTimeline(alice).minTimeMs == 50);
    CHECK(hub.replayTimeline(bob).maxTimeMs == 70);
    CHECK(alice.bound);
    CHECK(bob.bound);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSourceHubRapidSwitch() {
    const auto root = uniqueRoot("hub-rapid");
    writeLog(root / "a" / "run.log", {1000, 1010});
    writeLog(root / "b" / "run.log", {200, 210, 220});
    writeLog(root / "c" / "run.log", {9000});
    photon::SourceHub hub("127.0.0.1", 6500);
    std::string error;
    CHECK(hub.select(photon::ctrl::SourceKind::Synthetic, "", 0, "", error));
    photon::ReplayView view;
    std::uint64_t now = 1;
    const fs::path days[] = {root / "a", root / "b", root / "c", root / "b", root / "a"};
    for (const auto& day : days) {
        hub.resetReplay(view);
        CHECK(hub.openLog(day.string(), error));
        CHECK(waitBound(hub, view, now));
        hub.tickReplay(view, now);
        now += 20;
    }
    CHECK(hub.replayTimeline(view).minTimeMs == 1000);
    CHECK(hub.replayTimeline(view).maxTimeMs == 1010);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSyntheticSnapshotIsBoundedLookback() {
    photon::SourceHub hub("127.0.0.1", 6500);
    std::string error;
    CHECK(hub.select(photon::ctrl::SourceKind::Synthetic, "", 0, "", error));
    for (std::uint64_t t = 10; t <= 60000; t += 10) {
        hub.tick(t);
    }
    const auto blocks = hub.snapshotTelemetry({1, 2, 3, 4});
    CHECK(blocks.size() == 4);
    for (const auto& block : blocks) {
        CHECK(!block.timestamps.empty());
        CHECK(block.timestamps.size() == block.values.size());
        CHECK(block.timestamps.size() <= 2000);
        CHECK(block.timestamps.size() >= 1000);
        const double span = block.timestamps.back() - block.timestamps.front();
        CHECK(span <= 15020.0);
        CHECK(span >= 14000.0);
        CHECK(block.timestamps.back() == 60000.0);
    }
    const auto again = hub.snapshotTelemetry({1});
    CHECK(again.size() == 1);
    CHECK(again[0].values.back() == blocks[0].values.back());
}

void testSourceHubZipRejected() {
    const auto root = uniqueRoot("hub-zip");
    writeLog(root / "archive.zip" / "run.log", {9, 10});
    photon::SourceHub hub("127.0.0.1", 6500);
    std::string error;
    CHECK(hub.select(photon::ctrl::SourceKind::Synthetic, "", 0, "", error));
    CHECK(!hub.openLog((root / "archive.zip").string(), error));
    CHECK(error.find("zip") != std::string::npos);
    CHECK(!hub.hasLog());

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSourceHubSwitchBackToHubLogReplay() {
    const auto root = uniqueRoot("hub-back");
    writeLog(root / "primary" / "run.log", {4000, 4100});
    writeLog(root / "sidecar" / "run.log", {80, 90});
    photon::SourceHub hub("127.0.0.1", 6500);
    std::string error;
    CHECK(hub.select(photon::ctrl::SourceKind::LogReplay, "", 0, (root / "primary").string(), error));
    photon::ReplayView view;
    std::uint64_t now = 1;
    CHECK(waitBound(hub, view, now));
    CHECK(hub.replayTimeline(view).minTimeMs == 4000);

    hub.resetReplay(view);
    CHECK(hub.openLog((root / "sidecar").string(), error));
    CHECK(waitBound(hub, view, now));
    CHECK(hub.replayTimeline(view).minTimeMs == 80);
    CHECK(hub.kind() == photon::ctrl::SourceKind::LogReplay);

    hub.resetReplay(view);
    CHECK(hub.openLog((root / "primary").string(), error));
    CHECK(waitBound(hub, view, now));
    CHECK(hub.replayTimeline(view).minTimeMs == 4000);
    CHECK(hub.replayTimeline(view).maxTimeMs == 4100);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testTileCacheDropsOldLogSamples() {
    using photon::ui::TileCache;
    TileCache cache;
    cache.setTileStore(nullptr);
    cache.setSession("2026-07-23-01", "dbc", 5000, 7000);
    const auto q = cache.prepareQuery(1, 5000, 7000, 0, false, false);
    photon::ctrl::Message::HistorySeries s;
    s.signalId = 1;
    s.level = 0;
    s.points = 2;
    cache.ingestResult(photon::ctrl::historyResult(q, 0, 0, 5000, 7000, {s}, false));
    photon::proto::HistoryChunk c;
    c.queryId = q;
    c.signalId = 1;
    c.level = 0;
    c.fields = photon::proto::kHistRaw;
    c.flags = static_cast<std::uint16_t>(photon::proto::kHistDense | photon::proto::kHistFinal);
    c.timestamps = {5000.0, 7000.0};
    c.rawV = {120.0f, 121.0f};
    c.slotCount = 2;
    c.valueCount = 2;
    c.baseMs = 5000;
    cache.ingestChunk(c);
    CHECK(cache.gatherPlot(1, 5000, 7000, 0).t.size() == 2);

    cache.setSession("2026-07-18-01", "dbc", 100, 300);
    CHECK(cache.tileCount() == 0);
    const auto leftover = cache.gatherPlot(1, 100, 7000, 0);
    bool finite = false;
    for (double y : leftover.y) {
        if (std::isfinite(y)) {
            finite = true;
        }
    }
    CHECK(!finite);
    CHECK(cache.sessionId() == "2026-07-18-01");
    CHECK(cache.sessionMinMs() == 100);
    CHECK(cache.sessionMaxMs() == 300);
}

struct HubTicker {
    photon::SessionHub hub;
    std::atomic<bool> run{true};
    std::thread thread;

    HubTicker()
        : hub(0, "127.0.0.1", 6500, {}, {}, 0, photon::ctrl::SourceKind::Synthetic, {}, {}) {
        CHECK(hub.start());
        thread = std::thread([this] {
            while (run.load()) {
                hub.tick(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count()));
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }

    ~HubTicker() {
        run.store(false);
        if (thread.joinable()) {
            thread.join();
        }
        hub.stop();
    }
};

photon::ctrl::Message selectLog(const std::string& path) {
    auto msg = photon::ctrl::selectSource(photon::ctrl::SourceKind::LogReplay);
    msg.path = path;
    return msg;
}

void testSessionClientSwitchToOlderLog() {
    photon::net::Guard sockets;
    const auto root = uniqueRoot("ws-old");
    writeLog(root / "newer" / "run.log", {8000, 8100, 8200});
    writeLog(root / "older" / "run.log", {40, 50, 60});

    HubTicker live;
    auto sock = photon::net::connectTcp("127.0.0.1", live.hub.listenPort());
    CHECK(sock.valid());
    CHECK(handshake(sock));
    std::vector<std::uint8_t> buf;
    WsInbox inbox;
    CHECK(helloClient(sock, buf, inbox));

    CHECK(sendJson(sock, selectLog((root / "newer").string())));
    CHECK(waitAckPath(sock, buf, inbox, "newer"));

    inbox.timelines.clear();
    const auto tlNewDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool sawNew = false;
    while (std::chrono::steady_clock::now() < tlNewDeadline && !sawNew) {
        drain(sock, buf, inbox, std::chrono::milliseconds(50));
        for (const auto& st : inbox.timelines) {
            if (st.minTimeMs == 8000 && st.maxTimeMs == 8200) {
                sawNew = true;
            }
        }
    }
    CHECK(sawNew);

    CHECK(sendJson(sock, selectLog((root / "older").string())));
    CHECK(waitAckPath(sock, buf, inbox, "older"));
    CHECK(!inbox.error);

    bool sawOld = false;
    const auto tlOldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < tlOldDeadline && !sawOld) {
        drain(sock, buf, inbox, std::chrono::milliseconds(50));
        for (const auto& st : inbox.timelines) {
            if (st.minTimeMs == 40 && st.maxTimeMs == 60) {
                sawOld = true;
            }
        }
    }
    CHECK(sawOld);
    CHECK(live.hub.clientCount() == 1);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSessionTwoClientsSwitchDoesNotDropPeer() {
    photon::net::Guard sockets;
    const auto root = uniqueRoot("ws-two");
    writeLog(root / "a" / "run.log", {1000, 1100, 1200});
    writeLog(root / "b" / "run.log", {200, 300});

    HubTicker live;
    auto alice = photon::net::connectTcp("127.0.0.1", live.hub.listenPort());
    auto bob = photon::net::connectTcp("127.0.0.1", live.hub.listenPort());
    CHECK(alice.valid());
    CHECK(bob.valid());
    CHECK(handshake(alice));
    CHECK(handshake(bob));
    std::vector<std::uint8_t> aBuf;
    std::vector<std::uint8_t> bBuf;
    WsInbox aIn;
    WsInbox bIn;
    CHECK(helloClient(alice, aBuf, aIn));
    CHECK(helloClient(bob, bBuf, bIn));

    CHECK(sendJson(alice, selectLog((root / "a").string())));
    CHECK(waitAckPath(alice, aBuf, aIn, "a"));
    CHECK(sendJson(bob, selectLog((root / "a").string())));
    CHECK(waitAckPath(bob, bBuf, bIn, "a"));
    CHECK(live.hub.clientCount() == 2);

    CHECK(sendJson(alice, selectLog((root / "b").string())));
    CHECK(waitAckPath(alice, aBuf, aIn, "b"));
    CHECK(!aIn.error);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool bobSawB = false;
    while (std::chrono::steady_clock::now() < deadline && !bobSawB) {
        drain(bob, bBuf, bIn, std::chrono::milliseconds(40));
        if (const auto* ack = lastOf(bIn, photon::ctrl::Type::HelloAck)) {
            if (ack->activeSource.path.find("b") != std::string::npos) {
                bobSawB = true;
            }
        }
    }
    CHECK(bobSawB);
    CHECK(live.hub.clientCount() == 2);

    for (int i = 0; i < 30; ++i) {
        drain(alice, aBuf, aIn, std::chrono::milliseconds(10));
        drain(bob, bBuf, bIn, std::chrono::milliseconds(10));
    }
    CHECK(!aIn.error);
    CHECK(live.hub.clientCount() == 2);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSessionBadLogKeepsPointer() {
    photon::net::Guard sockets;
    const auto root = uniqueRoot("ws-bad");
    writeLog(root / "good" / "run.log", {1500, 1600});
    writeLog(root / "nolog.zip" / "run.log", {1});

    HubTicker live;
    auto sock = photon::net::connectTcp("127.0.0.1", live.hub.listenPort());
    CHECK(sock.valid());
    CHECK(handshake(sock));
    std::vector<std::uint8_t> buf;
    WsInbox inbox;
    CHECK(helloClient(sock, buf, inbox));
    CHECK(sendJson(sock, selectLog((root / "good").string())));
    CHECK(waitAckPath(sock, buf, inbox, "good"));

    inbox.error = false;
    CHECK(sendJson(sock, selectLog((root / "nolog.zip").string())));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !inbox.error) {
        drain(sock, buf, inbox, std::chrono::milliseconds(20));
    }
    CHECK(inbox.error);
    CHECK(inbox.errorCode == "bad_source");

    inbox.timelines.clear();
    bool stillGood = false;
    const auto tlDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < tlDeadline && !stillGood) {
        drain(sock, buf, inbox, std::chrono::milliseconds(50));
        for (const auto& st : inbox.timelines) {
            if (st.minTimeMs == 1500 && st.maxTimeMs == 1600) {
                stillGood = true;
            }
        }
    }
    CHECK(stillGood);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSessionLiveSnapsReplayClientToHubClock() {
    photon::net::Guard sockets;
    const auto root = uniqueRoot("ws-snap");
    writeLog(root / "day" / "run.log", {40, 50, 60});

    HubTicker live;
    auto alice = photon::net::connectTcp("127.0.0.1", live.hub.listenPort());
    auto bob = photon::net::connectTcp("127.0.0.1", live.hub.listenPort());
    CHECK(alice.valid());
    CHECK(bob.valid());
    CHECK(handshake(alice));
    CHECK(handshake(bob));
    std::vector<std::uint8_t> aBuf;
    std::vector<std::uint8_t> bBuf;
    WsInbox aIn;
    WsInbox bIn;
    CHECK(helloClient(alice, aBuf, aIn));
    CHECK(helloClient(bob, bBuf, bIn));

    CHECK(sendJson(alice, selectLog((root / "day").string())));
    CHECK(waitAckPath(alice, aBuf, aIn, "day"));

    aIn.timelines.clear();
    const auto replayDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool aliceOnReplay = false;
    while (std::chrono::steady_clock::now() < replayDeadline && !aliceOnReplay) {
        drain(alice, aBuf, aIn, std::chrono::milliseconds(40));
        for (const auto& st : aIn.timelines) {
            if (st.minTimeMs == 40 && st.maxTimeMs == 60) {
                aliceOnReplay = true;
            }
        }
    }
    CHECK(aliceOnReplay);

    CHECK(sendJson(alice, photon::ctrl::timelineControl(photon::ctrl::TimelineMode::Live, 0)));
    aIn.timelines.clear();
    const auto liveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool aliceLive = false;
    std::uint64_t aliceCursor = 0;
    while (std::chrono::steady_clock::now() < liveDeadline && !aliceLive) {
        drain(alice, aBuf, aIn, std::chrono::milliseconds(20));
        for (const auto& st : aIn.timelines) {
            if (st.mode == static_cast<std::uint8_t>(photon::proto::TimelineMode::Live) && st.minTimeMs != 40) {
                aliceLive = true;
                aliceCursor = st.cursorMs;
            }
        }
    }
    CHECK(aliceLive);

    bIn.timelines.clear();
    const auto bobDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool bobLive = false;
    std::uint64_t bobCursor = 0;
    while (std::chrono::steady_clock::now() < bobDeadline && !bobLive) {
        drain(bob, bBuf, bIn, std::chrono::milliseconds(40));
        for (const auto& st : bIn.timelines) {
            if (st.mode == static_cast<std::uint8_t>(photon::proto::TimelineMode::Live)) {
                bobLive = true;
                bobCursor = st.cursorMs;
            }
        }
    }
    CHECK(bobLive);
    if (aliceCursor > bobCursor) {
        CHECK(aliceCursor - bobCursor < 2000);
    } else {
        CHECK(bobCursor - aliceCursor < 2000);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testSessionLiveDetachThenOldLog() {
    photon::net::Guard sockets;
    const auto root = uniqueRoot("ws-live");
    writeLog(root / "old" / "run.log", {5, 15, 25});

    HubTicker live;
    auto sock = photon::net::connectTcp("127.0.0.1", live.hub.listenPort());
    CHECK(sock.valid());
    CHECK(handshake(sock));
    std::vector<std::uint8_t> buf;
    WsInbox inbox;
    CHECK(helloClient(sock, buf, inbox));
    CHECK(sendJson(sock, selectLog((root / "old").string())));
    CHECK(waitAckPath(sock, buf, inbox, "old"));

    auto liveMsg = photon::ctrl::selectSource(photon::ctrl::SourceKind::DaqTcp);
    liveMsg.host = "127.0.0.1";
    liveMsg.port = 6500;
    CHECK(sendJson(sock, liveMsg));
    const auto liveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool sawLive = false;
    while (std::chrono::steady_clock::now() < liveDeadline && !sawLive) {
        drain(sock, buf, inbox, std::chrono::milliseconds(20));
        if (const auto* ack = lastOf(inbox, photon::ctrl::Type::HelloAck)) {
            if (ack->activeSource.kind == photon::ctrl::SourceKind::Synthetic ||
                ack->activeSource.kind == photon::ctrl::SourceKind::DaqTcp) {
                sawLive = true;
            }
        }
    }
    CHECK(sawLive);

    inbox.error = false;
    CHECK(sendJson(sock, selectLog((root / "old").string())));
    CHECK(waitAckPath(sock, buf, inbox, "old"));
    CHECK(!inbox.error);

    std::error_code ec;
    fs::remove_all(root, ec);
}

}  // namespace

void runLogSwitchTests() {
    testSourceHubSwitchToOlderLog();
    testSourceHubBadPathKeepsCurrentLog();
    testSourceHubSamePathIsNoop();
    testSourceHubTwoViewsSurviveSwitch();
    testSourceHubRapidSwitch();
    testSyntheticSnapshotIsBoundedLookback();
    testSourceHubZipRejected();
    testSourceHubSwitchBackToHubLogReplay();
    testTileCacheDropsOldLogSamples();
    testSessionClientSwitchToOlderLog();
    testSessionTwoClientsSwitchDoesNotDropPeer();
    testSessionBadLogKeepsPointer();
    testSessionLiveSnapsReplayClientToHubClock();
    testSessionLiveDetachThenOldLog();
}
