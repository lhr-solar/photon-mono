#include "photon/canp.hpp"
#include "photon/control.hpp"
#include "photon/protocol.hpp"
#include "photon/video_udp.hpp"
#include "photon/ws_handshake.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

void runMuxTests();
void runDbcTests();
void runVsd1Tests();
void runVideoTests();
void runVideoIndexTests();
void runCliTests();
void runLiveFanoutTests();

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

void testLatestCanPerId() {
    photon::proto::CanMonitorItem a{};
    a.canId = 0x100;
    a.timestampMs = 1;
    a.data[0] = 1;
    photon::proto::CanMonitorItem b{};
    b.canId = 0x200;
    b.timestampMs = 2;
    b.data[0] = 2;
    photon::proto::CanMonitorItem a2{};
    a2.canId = 0x100;
    a2.timestampMs = 3;
    a2.data[0] = 9;
    const photon::proto::CanMonitorItem items[] = {a, b, a2};
    const auto out = photon::proto::latestCanPerId(items);
    CHECK(out.size() == 2);
    CHECK(out[0].canId == 0x100);
    CHECK(out[0].timestampMs == 3);
    CHECK(out[0].data[0] == 9);
    CHECK(out[1].canId == 0x200);
    const auto capped = photon::proto::latestCanPerId(items, 1);
    CHECK(capped.size() == 1);
    CHECK(capped[0].canId == 0x200);
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

    photon::ctrl::Message ackDays = photon::ctrl::helloAck({"log_replay"}, replayActive);
    photon::ctrl::Message::DriveDay day;
    day.date = "2026-07-23";
    day.path = R"(F:\lhr\_logs_extract\on_track\days\2026-07-23)";
    day.sessionCount = 10;
    day.durationS = 16600.0;
    ackDays.driveDays.push_back(day);
    const auto ackDaysText = photon::ctrl::serialize(ackDays);
    CHECK(ackDaysText.find("drive_days") != std::string::npos);
    CHECK(ackDaysText.find("2026-07-23") != std::string::npos);
    auto ackDaysBack = photon::ctrl::parse(ackDaysText);
    CHECK(ackDaysBack.has_value());
    CHECK(ackDaysBack->driveDays.size() == 1);
    CHECK(ackDaysBack->driveDays[0].date == "2026-07-23");
    CHECK(ackDaysBack->driveDays[0].sessionCount == 10);
    CHECK(ackDaysBack->driveDays[0].sessions.empty());
    photon::ctrl::Message::DriveSession stint;
    stint.id = "2026-07-23-08";
    stint.path = R"(F:\lhr\_logs_extract\on_track\days\2026-07-23\session-08.log)";
    stint.startMs = 1784841790574ull;
    stint.durationS = 1229.0;
    stint.speedMaxMps = 40.04;
    ackDays.driveDays[0].sessions.push_back(stint);
    auto ackStintBack = photon::ctrl::parse(photon::ctrl::serialize(ackDays));
    CHECK(ackStintBack.has_value());
    CHECK(ackStintBack->driveDays[0].sessions.size() == 1);
    CHECK(ackStintBack->driveDays[0].sessions[0].id == "2026-07-23-08");
    CHECK(ackStintBack->driveDays[0].sessions[0].startMs == 1784841790574ull);

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

    auto canOn = photon::ctrl::subscribeCan();
    auto canOnBack = photon::ctrl::parse(photon::ctrl::serialize(canOn));
    CHECK(canOnBack.has_value());
    CHECK(canOnBack->type == photon::ctrl::Type::SubscribeCan);
    auto canOff = photon::ctrl::unsubscribeCan();
    auto canOffBack = photon::ctrl::parse(photon::ctrl::serialize(canOff));
    CHECK(canOffBack.has_value());
    CHECK(canOffBack->type == photon::ctrl::Type::UnsubscribeCan);

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

    photon::ctrl::Message ackDbc = photon::ctrl::helloAck({"dbc"});
    ackDbc.dbcId = "carcan_telemcan";
    ackDbc.dbcPath = R"(F:\lhr\photon-next\assets\dbc\carcan_telemcan.dbc)";
    ackDbc.dbcs = {"carcan_telemcan"};
    const auto ackDbcText = photon::ctrl::serialize(ackDbc);
    CHECK(ackDbcText.find("\"dbc\"") != std::string::npos);
    CHECK(ackDbcText.find("carcan_telemcan") != std::string::npos);
    auto ackDbcBack = photon::ctrl::parse(ackDbcText);
    CHECK(ackDbcBack.has_value());
    CHECK(ackDbcBack->dbcId == "carcan_telemcan");
    CHECK(ackDbcBack->dbcPath == ackDbc.dbcPath);
    CHECK(ackDbcBack->dbcs.size() == 1);
    CHECK(ackDbcBack->dbcs[0] == "carcan_telemcan");

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

    auto vloc = photon::ctrl::videoLocate(1784843212345ull, 2, 8);
    auto vlocBack = photon::ctrl::parse(photon::ctrl::serialize(vloc));
    CHECK(vlocBack.has_value());
    CHECK(vlocBack->type == photon::ctrl::Type::VideoLocate);
    CHECK(vlocBack->startMs == 1784843212345ull);
    CHECK(vlocBack->videoStreamId == 2);
    CHECK(vlocBack->videoPrefetchGops == 8);

    auto vclk = photon::ctrl::videoClock(-137);
    auto vclkBack = photon::ctrl::parse(photon::ctrl::serialize(vclk));
    CHECK(vclkBack.has_value());
    CHECK(vclkBack->type == photon::ctrl::Type::VideoClock);
    CHECK(vclkBack->videoClockOffsetMs == -137);

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

    auto hq = photon::ctrl::historyQuery(17, "20260723-160000-03", 1000, 5000, {1, 2}, 2000, 4096);
    auto hqBack = photon::ctrl::parse(photon::ctrl::serialize(hq));
    CHECK(hqBack.has_value());
    CHECK(hqBack->type == photon::ctrl::Type::HistoryQuery);
    CHECK(hqBack->queryId == 17);
    CHECK(hqBack->sessionId == "20260723-160000-03");
    CHECK(hqBack->fromMs == 1000);
    CHECK(hqBack->toMs == 5000);
    CHECK(hqBack->signalIds.size() == 2);
    CHECK(hqBack->creditBytes == 4096);

    photon::ctrl::Message::HistorySeries hs;
    hs.signalId = 1;
    hs.points = 10;
    hs.level = 2;
    hs.bucketMs = 1000;
    hs.prior.present = true;
    hs.prior.t = 900;
    hs.prior.v = 3.5;
    auto hr = photon::ctrl::historyResult(17, 2, 1000, 1000, 5000, {hs}, false);
    auto hrBack = photon::ctrl::parse(photon::ctrl::serialize(hr));
    CHECK(hrBack.has_value());
    CHECK(hrBack->type == photon::ctrl::Type::HistoryResult);
    CHECK(hrBack->historyLevel == 2);
    CHECK(hrBack->historySeries.size() == 1);
    CHECK(hrBack->historySeries[0].level == 2);
    CHECK(hrBack->historySeries[0].bucketMs == 1000);
    CHECK(hrBack->historySeries[0].prior.present);
    CHECK(hrBack->historySeries[0].prior.t == 900);

    auto hc = photon::ctrl::parse(photon::ctrl::serialize(photon::ctrl::historyCancel(17)));
    CHECK(hc.has_value());
    CHECK(hc->type == photon::ctrl::Type::HistoryCancel);
    CHECK(hc->queryId == 17);

    auto cl = photon::ctrl::parse(photon::ctrl::serialize(photon::ctrl::catalogList()));
    CHECK(cl.has_value());
    CHECK(cl->type == photon::ctrl::Type::CatalogList);

    photon::ctrl::Message::CatalogSession sess;
    sess.id = "20260904-090000-00";
    sess.path = "/tsdb/20260904-090000-00";
    sess.minMs = 1000;
    sess.maxMs = 50000;
    sess.dbcHash = "deadbeef";
    sess.sampleCount = 42;
    sess.hasVideo = true;
    auto cr = photon::ctrl::catalogResult({sess});
    const auto crText = photon::ctrl::serialize(cr);
    CHECK(crText.find("catalog_result") != std::string::npos);
    CHECK(crText.find("dbc_hash") != std::string::npos);
    CHECK(crText.find("min_ms") != std::string::npos);
    auto crBack = photon::ctrl::parse(crText);
    CHECK(crBack.has_value());
    CHECK(crBack->type == photon::ctrl::Type::CatalogResult);
    CHECK(crBack->catalogSessions.size() == 1);
    CHECK(crBack->catalogSessions[0].id == "20260904-090000-00");
    CHECK(crBack->catalogSessions[0].minMs == 1000);
    CHECK(crBack->catalogSessions[0].maxMs == 50000);
    CHECK(crBack->catalogSessions[0].dbcHash == "deadbeef");
    CHECK(crBack->catalogSessions[0].sampleCount == 42);
    CHECK(crBack->catalogSessions[0].hasVideo);
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

void testVideoUdpAssemble() {
    photon::video::ServerFrame frame;
    frame.timestampMs = 99;
    frame.flags = photon::video::kFlagKeyframe;
    frame.payload.assign(2500, 0x65);
    // Server fragments at 1200 B; emulate two packets then the tail.
    photon::video::UdpPacket a;
    a.frameSeq = 3;
    a.fragIndex = 0;
    a.fragCount = 3;
    a.timestampMs = 99;
    a.flags = photon::video::kFlagKeyframe;
    a.payload.assign(frame.payload.begin(), frame.payload.begin() + 1200);
    photon::video::UdpPacket b = a;
    b.fragIndex = 1;
    b.payload.assign(frame.payload.begin() + 1200, frame.payload.begin() + 2400);
    photon::video::UdpPacket c = a;
    c.fragIndex = 2;
    c.payload.assign(frame.payload.begin() + 2400, frame.payload.end());
    photon::video::UdpAssembler asmblr;
    CHECK(!asmblr.add(a).has_value());
    CHECK(!asmblr.add(b).has_value());
    auto got = asmblr.add(c);
    CHECK(got.has_value());
    CHECK(got->timestampMs == 99);
    CHECK(got->payload.size() == 2500);
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
    testLatestCanPerId();
    testTimelineRoundTrip();
    testVideoFrameRoundTrip();
    testVideoUdpAssemble();
    testControlJsonRoundTrip();
    testCanpRoundTrip();
    testWsAcceptKey();
    runMuxTests();
    runDbcTests();
    runVsd1Tests();
    runVideoTests();
    runVideoIndexTests();
    runCliTests();
    runLiveFanoutTests();

    std::fprintf(stdout, "photon-tests: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
