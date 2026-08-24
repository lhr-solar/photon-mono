#include "photon/canp.hpp"
#include "photon/control.hpp"
#include "photon/protocol.hpp"
#include "photon/ws_handshake.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

void runTrackSimTests();
void runDbcTests();
void runPcanTests();
void runVsd1Tests();
void runVideoTests();
void runCliTests();

namespace {

void testHeaderPacking() {
    photon::proto::FrameHeader header{};
    header.magic = photon::proto::kMagic;
    header.version = photon::proto::kVersion;
    header.msgType = static_cast<std::uint8_t>(photon::proto::MsgType::TelemetryBatch);
    header.sequence = 0x11223344u;
    header.payloadLen = 0xAABBCCDDu;

    const auto bytes = photon::proto::encodeHeader(header);
    CHECK(bytes.size() == 12);
    CHECK(bytes[0] == 0x57);
    CHECK(bytes[1] == 0x50);
    CHECK(bytes[2] == 1);
    CHECK(bytes[3] == 0x01);
    CHECK(bytes[4] == 0x44);
    CHECK(bytes[5] == 0x33);
    CHECK(bytes[6] == 0x22);
    CHECK(bytes[7] == 0x11);

    const auto decoded = photon::proto::decodeHeader(bytes);
    CHECK(decoded.has_value());
    CHECK(decoded->magic == photon::proto::kMagic);
    CHECK(decoded->version == 1);
    CHECK(decoded->msgType == 0x01);
    CHECK(decoded->sequence == 0x11223344u);
    CHECK(decoded->payloadLen == 0xAABBCCDDu);

    CHECK(sizeof(photon::proto::FrameHeader) == 12);
    CHECK(sizeof(photon::proto::CanMonitorItem) == 24);
    CHECK(sizeof(photon::proto::TimelineStatus) == 33);
    CHECK(sizeof(photon::proto::VideoFrameHeader) == 12);
    CHECK(photon::proto::kVideoFrameHeaderSize == 12);
}

void testTelemetryRoundTrip() {
    photon::proto::TelemetryBlock a;
    a.signalId = 7;
    a.reserved = 0xBEEF;
    a.timestamps = {1000.5, 1010.25};
    a.values = {3.5, -1.25};
    photon::proto::TelemetryBlock b;
    b.signalId = 9;
    b.timestamps = {50.0};
    b.values = {42.0};

    const std::vector<photon::proto::TelemetryBlock> in{a, b};
    const auto payload = photon::proto::encodeTelemetryBatch(in);
    const auto frame = photon::proto::encodeFrame(photon::proto::MsgType::TelemetryBatch, 99, payload);
    const auto decodedFrame = photon::proto::decodeFrame(frame);
    CHECK(decodedFrame.has_value());
    CHECK(decodedFrame->header.sequence == 99);
    CHECK(decodedFrame->header.msgType == 0x01);

    const auto out = photon::proto::decodeTelemetryBatch(decodedFrame->payload);
    CHECK(out.has_value());
    CHECK(out->size() == 2);
    CHECK((*out)[0].signalId == 7);
    CHECK((*out)[0].reserved == 0xBEEF);
    CHECK((*out)[0].timestamps.size() == 2);
    CHECK((*out)[0].timestamps[0] == 1000.5);
    CHECK((*out)[0].values[1] == -1.25);
    CHECK((*out)[1].signalId == 9);
    CHECK((*out)[1].values[0] == 42.0);
}

void testCanMonitorRoundTrip() {
    photon::proto::CanMonitorItem item{};
    item.canId = 0x7FFu;
    item.dlc = 8;
    item.flags = 0x01;
    item.reserved = 0x1234;
    item.timestampMs = 0x0102030405060708ull;
    for (int i = 0; i < 8; ++i) {
        item.data[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    const auto payload = photon::proto::encodeCanMonitorBatch(std::span<const photon::proto::CanMonitorItem>(&item, 1));
    CHECK(payload.size() == 24);
    const auto out = photon::proto::decodeCanMonitorBatch(payload);
    CHECK(out.has_value());
    CHECK(out->size() == 1);
    CHECK((*out)[0].canId == 0x7FFu);
    CHECK((*out)[0].dlc == 8);
    CHECK((*out)[0].flags == 0x01);
    CHECK((*out)[0].reserved == 0x1234);
    CHECK((*out)[0].timestampMs == 0x0102030405060708ull);
    CHECK((*out)[0].data[7] == 0xA7);
}

void testTimelineRoundTrip() {
    photon::proto::TimelineStatus status{};
    status.mode = static_cast<std::uint8_t>(photon::proto::TimelineMode::Live);
    status.cursorMs = 123;
    status.minTimeMs = 1;
    status.maxTimeMs = 999;
    status.totalBatches = 42;
    const auto payload = photon::proto::encodeTimelineStatus(status);
    CHECK(payload.size() == 33);
    const auto out = photon::proto::decodeTimelineStatus(payload);
    CHECK(out.has_value());
    CHECK(out->mode == 3);
    CHECK(out->cursorMs == 123);
    CHECK(out->minTimeMs == 1);
    CHECK(out->maxTimeMs == 999);
    CHECK(out->totalBatches == 42);
}

void testVideoFrameRoundTrip() {
    photon::proto::VideoFrame frame;
    frame.timestampMs = 0x0102030405060708ull;
    frame.flags = photon::proto::kVideoFlagKeyframe | photon::proto::kVideoFlagConfig;
    frame.reserved[0] = 0xAA;
    frame.reserved[1] = 0xBB;
    frame.reserved[2] = 0xCC;
    frame.nal = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E};

    const auto payload = photon::proto::encodeVideoFrame(frame);
    CHECK(payload.size() == 12 + frame.nal.size());
    CHECK(payload[8] == (photon::proto::kVideoFlagKeyframe | photon::proto::kVideoFlagConfig));
    CHECK(payload[9] == 0xAA);
    CHECK(payload[10] == 0xBB);
    CHECK(payload[11] == 0xCC);
    // timestamp little-endian
    CHECK(payload[0] == 0x08);
    CHECK(payload[7] == 0x01);

    const auto decoded = photon::proto::decodeVideoFrame(payload);
    CHECK(decoded.has_value());
    CHECK(decoded->timestampMs == 0x0102030405060708ull);
    CHECK(decoded->flags == frame.flags);
    CHECK(decoded->reserved[2] == 0xCC);
    CHECK(decoded->nal == frame.nal);
    CHECK(photon::proto::videoKeyframe(decoded->flags));
    CHECK(photon::proto::videoConfig(decoded->flags));

    const auto wrapped = photon::proto::encodeFrame(photon::proto::MsgType::VideoFrame, 7, payload);
    const auto decodedFrame = photon::proto::decodeFrame(wrapped);
    CHECK(decodedFrame.has_value());
    CHECK(decodedFrame->header.msgType == 0x04);
    CHECK(decodedFrame->header.sequence == 7);

    CHECK(!photon::proto::decodeVideoFrame(std::span<const std::uint8_t>(payload.data(), 11)).has_value());
}

void testControlJsonRoundTrip() {
    auto hello = photon::ctrl::hello("wasm-ui");
    const auto helloText = photon::ctrl::serialize(hello);
    auto helloBack = photon::ctrl::parse(helloText);
    CHECK(helloBack.has_value());
    CHECK(helloBack->type == photon::ctrl::Type::Hello);
    CHECK(helloBack->protocolVersion == 1);
    CHECK(helloBack->client == "wasm-ui");

    auto ack = photon::ctrl::helloAck({"synthetic", "daq_tcp"});
    auto ackBack = photon::ctrl::parse(photon::ctrl::serialize(ack));
    CHECK(ackBack.has_value());
    CHECK(ackBack->features.size() == 2);
    CHECK(ackBack->features[0] == "synthetic");
    CHECK(ackBack->activeSource.kind == photon::ctrl::SourceKind::None);

    photon::ctrl::Message::ActiveSource replayActive;
    replayActive.kind = photon::ctrl::SourceKind::LogReplay;
    replayActive.path = R"(C:\Users\alper\Downloads\logs\logs)";
    auto ackReplay = photon::ctrl::helloAck({"synthetic", "daq_tcp", "log_replay"}, replayActive);
    auto ackReplayText = photon::ctrl::serialize(ackReplay);
    CHECK(ackReplayText.find("active_source") != std::string::npos);
    CHECK(ackReplayText.find("log_replay") != std::string::npos);
    auto ackReplayBack = photon::ctrl::parse(ackReplayText);
    CHECK(ackReplayBack.has_value());
    CHECK(ackReplayBack->type == photon::ctrl::Type::HelloAck);
    CHECK(ackReplayBack->activeSource.kind == photon::ctrl::SourceKind::LogReplay);
    CHECK(ackReplayBack->activeSource.path == replayActive.path);
    CHECK(ackReplayBack->activeSource.port == 0);

    photon::ctrl::Message::ActiveSource daqActive;
    daqActive.kind = photon::ctrl::SourceKind::DaqTcp;
    daqActive.host = "10.0.0.8";
    daqActive.port = 6500;
    auto ackDaqBack = photon::ctrl::parse(photon::ctrl::serialize(photon::ctrl::helloAck({"daq_tcp"}, daqActive)));
    CHECK(ackDaqBack.has_value());
    CHECK(ackDaqBack->activeSource.kind == photon::ctrl::SourceKind::DaqTcp);
    CHECK(ackDaqBack->activeSource.host == "10.0.0.8");
    CHECK(ackDaqBack->activeSource.port == 6500);

    auto src = photon::ctrl::selectSource(photon::ctrl::SourceKind::DaqTcp);
    src.host = "127.0.0.1";
    src.port = 6500;
    auto srcBack = photon::ctrl::parse(photon::ctrl::serialize(src));
    CHECK(srcBack.has_value());
    CHECK(srcBack->source == photon::ctrl::SourceKind::DaqTcp);
    CHECK(srcBack->host == "127.0.0.1");
    CHECK(srcBack->port == 6500);

    auto replay = photon::ctrl::selectSource(photon::ctrl::SourceKind::LogReplay);
    replay.path = "C:/logs/run.bin";
    auto replayBack = photon::ctrl::parse(photon::ctrl::serialize(replay));
    CHECK(replayBack.has_value());
    CHECK(replayBack->path == "C:/logs/run.bin");

    auto sub = photon::ctrl::subscribe({1, 2, 4});
    auto subBack = photon::ctrl::parse(photon::ctrl::serialize(sub));
    CHECK(subBack.has_value());
    CHECK(subBack->signalIds.size() == 3);
    CHECK(subBack->signalIds[2] == 4);

    auto tl = photon::ctrl::timelineControl(photon::ctrl::TimelineMode::Pause, 5000);
    auto tlBack = photon::ctrl::parse(photon::ctrl::serialize(tl));
    CHECK(tlBack.has_value());
    CHECK(tlBack->timelineMode == photon::ctrl::TimelineMode::Pause);
    CHECK(tlBack->cursorMs == 5000);
    CHECK(tlBack->speed == 1.0);

    auto tlFast = photon::ctrl::timelineControl(photon::ctrl::TimelineMode::Play, 1200, 2.5);
    auto tlFastBack = photon::ctrl::parse(photon::ctrl::serialize(tlFast));
    CHECK(tlFastBack.has_value());
    CHECK(tlFastBack->speed == 2.5);

    auto dbc = photon::ctrl::selectDbc("carcan_telemcan", true);
    auto dbcBack = photon::ctrl::parse(photon::ctrl::serialize(dbc));
    CHECK(dbcBack.has_value());
    CHECK(dbcBack->type == photon::ctrl::Type::SelectDbc);
    CHECK(dbcBack->dbcId == "carcan_telemcan");

    photon::ctrl::CatalogSignal catSig;
    catSig.id = 42;
    catSig.message = "EngineData";
    catSig.name = "RPM";
    catSig.unit = "rpm";
    catSig.min = 0;
    catSig.max = 8000;
    auto cat = photon::ctrl::signalCatalog({catSig});
    auto catBack = photon::ctrl::parse(photon::ctrl::serialize(cat));
    CHECK(catBack.has_value());
    CHECK(catBack->type == photon::ctrl::Type::SignalCatalog);
    CHECK(catBack->catalog.size() == 1);
    CHECK(catBack->catalog[0].id == 42);
    CHECK(catBack->catalog[0].message == "EngineData");
    CHECK(catBack->catalog[0].name == "RPM");

    auto arm = photon::ctrl::txArmRequest(true);
    auto armBack = photon::ctrl::parse(photon::ctrl::serialize(arm));
    CHECK(armBack.has_value());
    CHECK(armBack->armed);

    auto status = photon::ctrl::txArmStatus(false, "heartbeat_timeout");
    auto statusBack = photon::ctrl::parse(photon::ctrl::serialize(status));
    CHECK(statusBack.has_value());
    CHECK(!statusBack->armed);
    CHECK(statusBack->reason == "heartbeat_timeout");

    auto err = photon::ctrl::errorAlert("not_implemented", "pcan later");
    auto errBack = photon::ctrl::parse(photon::ctrl::serialize(err));
    CHECK(errBack.has_value());
    CHECK(errBack->code == "not_implemented");

    auto vsub = photon::ctrl::videoSubscribe(photon::ctrl::VideoMode::Replay, 1710000000000ull);
    vsub.host = "127.0.0.1";
    vsub.port = 6700;
    auto vsubBack = photon::ctrl::parse(photon::ctrl::serialize(vsub));
    CHECK(vsubBack.has_value());
    CHECK(vsubBack->type == photon::ctrl::Type::VideoSubscribe);
    CHECK(vsubBack->videoMode == photon::ctrl::VideoMode::Replay);
    CHECK(vsubBack->startMs == 1710000000000ull);
    CHECK(vsubBack->host == "127.0.0.1");
    CHECK(vsubBack->port == 6700);

    auto vstop = photon::ctrl::parse(photon::ctrl::serialize(photon::ctrl::videoStop()));
    CHECK(vstop.has_value());
    CHECK(vstop->type == photon::ctrl::Type::VideoStop);

    auto vst = photon::ctrl::videoStatus(true, "10.0.0.2", 6700, photon::ctrl::VideoMode::Live, 42, 9, 3);
    auto vstBack = photon::ctrl::parse(photon::ctrl::serialize(vst));
    CHECK(vstBack.has_value());
    CHECK(vstBack->type == photon::ctrl::Type::VideoStatus);
    CHECK(vstBack->videoConnected);
    CHECK(vstBack->host == "10.0.0.2");
    CHECK(vstBack->port == 6700);
    CHECK(vstBack->videoMode == photon::ctrl::VideoMode::Live);
    CHECK(vstBack->lastTsMs == 42);
    CHECK(vstBack->videoFrames == 9);
    CHECK(vstBack->videoDropped == 3);
}

void testCanpRoundTrip() {
    photon::canp::Batch batch{};
    batch.seq = 7;
    batch.timestamp = 0x0102030405060708ull;
    batch.count = 2;
    batch.packets[0].canId = 0x7FF;
    batch.packets[0].dlc = 8;
    for (int i = 0; i < 8; ++i) {
        batch.packets[0].data[i] = static_cast<std::uint8_t>(0x10 + i);
        batch.packets[0].dt[i] = static_cast<std::uint16_t>(100 + i);
    }
    batch.packets[1].canId = 0x7FE;
    batch.packets[1].dlc = 3;
    batch.packets[1].data[0] = 0xAA;
    batch.packets[1].data[1] = 0xBB;
    batch.packets[1].data[2] = 0xCC;

    const auto wire = photon::canp::encodeBatch(batch);
    CHECK(wire.size() == 20 + 2 * 29);
    CHECK(wire[0] == 'C');
    CHECK(wire[1] == 'A');
    CHECK(wire[2] == 'N');
    CHECK(wire[3] == '1');
    CHECK(wire[4] == 0);
    CHECK(wire[5] == 3);

    photon::canp::Batch decoded{};
    std::size_t consumed = 0;
    CHECK(photon::canp::parseBatch(wire, decoded, consumed) == photon::canp::ParseStatus::Ok);
    CHECK(consumed == wire.size());
    CHECK(decoded.seq == 7);
    CHECK(decoded.timestamp == 0x0102030405060708ull);
    CHECK(decoded.count == 2);
    CHECK(decoded.packets[0].canId == 0x7FF);
    CHECK(decoded.packets[0].dt[7] == 107);
    CHECK(decoded.packets[1].canId == 0x7FE);
    CHECK(decoded.packets[1].dlc == 3);
    CHECK(decoded.packets[1].data[2] == 0xCC);

    auto incomplete = photon::canp::parseBatch(std::span<const std::uint8_t>(wire.data(), 10), decoded, consumed);
    CHECK(incomplete == photon::canp::ParseStatus::Incomplete);

    std::vector<std::uint8_t> bad = wire;
    bad[0] = 'X';
    CHECK(photon::canp::parseBatch(bad, decoded, consumed) == photon::canp::ParseStatus::BadMagic);

    photon::canp::StreamParser parser;
    parser.feed(std::span<const std::uint8_t>(wire.data(), 15));
    CHECK(!parser.take().has_value());
    parser.feed(std::span<const std::uint8_t>(wire.data() + 15, wire.size() - 15));
    auto streamed = parser.take();
    CHECK(streamed.has_value());
    CHECK(streamed->seq == 7);
}

void testWsAcceptKey() {
    // RFC 6455 example key / accept pair.
    const std::string accept = photon::ws::acceptKey("dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

}  // namespace

int main() {
    testHeaderPacking();
    testTelemetryRoundTrip();
    testCanMonitorRoundTrip();
    testTimelineRoundTrip();
    testVideoFrameRoundTrip();
    testControlJsonRoundTrip();
    testCanpRoundTrip();
    testWsAcceptKey();
    runTrackSimTests();
    runDbcTests();
    runPcanTests();
    runVsd1Tests();
    runVideoTests();
    runCliTests();

    std::fprintf(stdout, "photon-tests: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
