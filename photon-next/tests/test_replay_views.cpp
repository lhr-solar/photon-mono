#include "photon/canp.hpp"
#include "photon/dbc.hpp"
#include "photon/endian.hpp"
#include "photon/source.hpp"
#include "photon/vsd1.hpp"
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

void runReplayViewTests();

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

}  // namespace

void runReplayViewTests() {
    const auto dir = fs::temp_directory_path() / "photon-replay-views";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::vector<std::uint8_t> bytes;
        auto a = encodeVsd1(1, makeBatch(1, 1000, 0x100, 0x11));
        auto b = encodeVsd1(2, makeBatch(2, 2500, 0x101, 0x22));
        auto c = encodeVsd1(3, makeBatch(3, 4000, 0x102, 0x33));
        bytes.insert(bytes.end(), a.begin(), a.end());
        bytes.insert(bytes.end(), b.begin(), b.end());
        bytes.insert(bytes.end(), c.begin(), c.end());
        std::ofstream out(dir / "run.log", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    photon::SourceHub hub("127.0.0.1", 6500);
    std::string error;
    CHECK(hub.select(photon::ctrl::SourceKind::LogReplay, "", 0, dir.string(), error));
    CHECK(hub.replayActive());

    photon::ReplayView alice;
    photon::ReplayView bob;
    bool ready = false;
    std::uint64_t now = 1;
    for (int i = 0; i < 200; ++i) {
        hub.tick(now);
        hub.attachReplay(alice);
        hub.attachReplay(bob);
        if (alice.bound && bob.bound) {
            ready = true;
            break;
        }
        now += 20;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(ready);

    hub.controlReplay(alice, photon::ctrl::TimelineMode::Pause, 1000, 1.0);
    hub.controlReplay(bob, photon::ctrl::TimelineMode::Pause, 4000, 1.0);
    const auto ta = hub.replayTimeline(alice);
    const auto tb = hub.replayTimeline(bob);
    CHECK(ta.cursorMs == 1000);
    CHECK(tb.cursorMs == 4000);
    CHECK(ta.cursorMs != tb.cursorMs);

    hub.tickReplay(alice, now + 100);
    hub.tickReplay(bob, now + 100);
    CHECK(hub.replayTimeline(alice).cursorMs == 1000);
    CHECK(hub.replayTimeline(bob).cursorMs == 4000);

    hub.controlTimeline(photon::ctrl::TimelineMode::Live, 1000, 1.0);
    const auto liveTl = hub.timeline();
    CHECK(liveTl.cursorMs == 4000);

    photon::SourceHub live("127.0.0.1", 6500);
    CHECK(live.select(photon::ctrl::SourceKind::DaqTcp, "127.0.0.1", 6500, "", error));
    CHECK(live.kind() == photon::ctrl::SourceKind::DaqTcp);
    CHECK(live.openLog(dir.string(), error));
    CHECK(live.kind() == photon::ctrl::SourceKind::DaqTcp);
    CHECK(live.hasLog());
    photon::ReplayView pointer;
    bool logReady = false;
    for (int i = 0; i < 200; ++i) {
        live.tick(now);
        live.attachReplay(pointer);
        if (pointer.bound) {
            logReady = true;
            break;
        }
        now += 20;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(logReady);
    live.controlReplay(pointer, photon::ctrl::TimelineMode::Play, 0, 1.0);
    CHECK(live.replayTimeline(pointer).cursorMs == 1000);
    CHECK(live.replayTimeline(pointer).mode ==
          static_cast<std::uint8_t>(photon::proto::TimelineMode::Play));
    live.controlReplay(pointer, photon::ctrl::TimelineMode::Live, 0, 1.0);
    CHECK(!pointer.bound);
    CHECK(live.kind() == photon::ctrl::SourceKind::DaqTcp);

    fs::remove_all(dir, ec);

    const char* fixture = R"(
BO_ 256 EngineData: 8 Vector
 SG_ RPM : 0|16@1+ (0.25,0) [0|8000] "rpm" Vector
 SG_ Temp : 16|8@1- (1,-40) [-40|215] "C" Vector
)";
    auto db = photon::dbc::parse(fixture);
    CHECK(db.has_value());
    const auto rpmId = photon::dbc::signalIdOf("EngineData", "RPM");
    const auto tempId = photon::dbc::signalIdOf("EngineData", "Temp");
    photon::DecodeBuffer buf;
    buf.setDatabase(&*db);
    photon::proto::CanMonitorItem item{};
    item.canId = 256;
    item.dlc = 8;
    item.timestampMs = 1000;
    item.data[0] = 0xE8;
    item.data[1] = 0x03;
    item.data[2] = 100;
    buf.ingest(item);
    CHECK(buf.ring(rpmId) != nullptr);
    CHECK(buf.ring(tempId) != nullptr);

    buf.setWanted({rpmId});
    CHECK(buf.ring(rpmId) != nullptr);
    CHECK(buf.ring(tempId) == nullptr);
    item.timestampMs = 2000;
    buf.ingest(item);
    double rpmLatest = 0.0;
    CHECK(buf.latest(rpmId, rpmLatest));
    CHECK(near(rpmLatest, 250.0));
    double tempLatest = 0.0;
    CHECK(!buf.latest(tempId, tempLatest));

    buf.setWanted({});
    item.timestampMs = 3000;
    buf.ingest(item);
    CHECK(buf.ring(rpmId) == nullptr);
}
