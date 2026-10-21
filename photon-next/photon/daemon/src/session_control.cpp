#include "photon/session.hpp"
#include "photon/array_sim.hpp"

#include "photon/log_sessions.hpp"
#include "photon/video_index.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace photon {
namespace {

constexpr std::uint64_t kTxWatchdogMs = 3000;

bool safeDbcId(std::string_view id) {
    if (id.empty() || id.size() > 96) {
        return false;
    }
    for (char c : id) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

}  // namespace

void SessionHub::refreshDriveDays() {
    driveDays_.clear();
    const auto listed = logsess::listDriveDays(startupLogPath_);
    driveDays_.reserve(listed.size());
    for (const auto& day : listed) {
        ctrl::Message::DriveDay item;
        item.date = day.date;
        item.path = day.path;
        item.sessionCount = day.sessionCount;
        item.durationS = day.durationS;
        for (const auto& sess : day.sessions) {
            ctrl::Message::DriveSession row;
            row.id = sess.id;
            row.path = sess.path;
            row.startMs = sess.startMs;
            row.durationS = sess.durationS;
            row.speedMaxMps = sess.speedMaxMps;
            item.sessions.push_back(std::move(row));
        }
        driveDays_.push_back(std::move(item));
    }
    if (!driveDays_.empty()) {
        std::fprintf(stdout, "[photon-backend] drive days %zu", driveDays_.size());
        for (const auto& day : driveDays_) {
            std::fprintf(stdout, " %s", day.date.c_str());
        }
        std::fprintf(stdout, "\n");
    }
}

ctrl::Message SessionHub::helloAckMessage() const {
    auto ack = ctrl::helloAck({"daq_tcp", "log_replay", "pcan", "dbc", "track_sim", "video", "history",
                               "video_locate", "live_tail", "video_clock", "analysis_compute", "catalog"},
                              sources_.activeSource());
    if (array_sim::available()) {
        ack.features.push_back("array_sim");
    }
    ack.driveDays = driveDays_;
    if (dbcLoaded_) {
        ack.dbcId = dbc_.label;
        ack.dbcPath = dbc_.path;
    }
    ack.dbcs = dbc::listBundledIds(assetsDir_);
    return ack;
}

ctrl::Message SessionHub::helloAckMessage(const ClientSession& client) const {
    auto ack = helloAckMessage();
    if (client.logPointer && sources_.hasLog()) {
        ack.activeSource.kind = ctrl::SourceKind::LogReplay;
        ack.activeSource.path = sources_.logPath();
        ack.activeSource.host.clear();
        ack.activeSource.port = 0;
    }
    return ack;
}

bool SessionHub::applySelectSource(ctrl::SourceKind kind, const std::string& host, std::uint16_t port,
                                   const std::string& path, std::string& error, const pcan::Config& pcanCfg) {
    std::string usePath = path;
    if (kind == ctrl::SourceKind::LogReplay) {
        const std::string hint = path.empty() ? startupLogPath_ : path;
        usePath = logsess::resolveReplayPath(hint);
        if (!logsess::directoryHasLogs(usePath)) {
            error = "no VSD1 .log files at path (point at a day folder or on_track catalog)";
            return false;
        }
    }
    const auto prevKind = sources_.kind();
    const auto prevPath = sources_.activeSource().path;
    if (!sources_.select(kind, host, port, usePath, error, pcanCfg)) {
        return false;
    }
    const bool replayChanged =
        kind != prevKind || (kind == ctrl::SourceKind::LogReplay && usePath != prevPath);
    if (replayChanged) {
        resetClientReplays();
    }
    stopLiveWriter();
    lastTelemetryMs_ = 0;
    lastCanBurstMs_ = 0;
    lastTimelineMs_ = 0;
    return true;
}

void SessionHub::sendControl(ClientSession& client, const ctrl::Message& msg) {
    const auto before = client.conn.outboundBytes();
    client.conn.queueText(ctrl::serialize(msg));
    noteQueued(client, msg.type == ctrl::Type::HistoryResult, before);
}

void SessionHub::disarm(ClientSession& client, std::string reason) {
    if (!client.txArmed && reason != "client_disconnect") {
        sendControl(client, ctrl::txArmStatus(false, reason));
        return;
    }
    client.txArmed = false;
    std::fprintf(stdout, "[photon-backend] TX disarmed (%s) client=%s\n", reason.c_str(), client.label.c_str());
    if (reason != "client_disconnect") {
        sendControl(client, ctrl::txArmStatus(false, std::move(reason)));
    }
}

void SessionHub::handleText(ClientSession& client, std::string_view text, std::uint64_t nowMs) {
    std::string error;
    auto msg = ctrl::parse(text, &error);
    if (!msg) {
        sendControl(client, ctrl::errorAlert("bad_message", error.empty() ? "invalid JSON control" : error));
        return;
    }
    client.lastHeartbeatMs = nowMs;

    switch (msg->type) {
        case ctrl::Type::Hello:
            if (msg->protocolVersion != ctrl::kProtocolVersion) {
                sendControl(client, ctrl::errorAlert("protocol_mismatch", "server speaks photon-wire v1"));
                return;
            }
            client.helloOk = true;
            sendControl(client, helloAckMessage(client));
            sendCatalog(client);
            std::fprintf(stdout, "[photon-backend] hello from %s (%s)\n", client.label.c_str(),
                         msg->client.empty() ? "ui" : msg->client.c_str());
            break;
        case ctrl::Type::SelectSource: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            if (msg->source == ctrl::SourceKind::LogReplay) {
                std::string logError;
                if (!attachClientLog(client, msg->path, logError)) {
                    sendControl(client, ctrl::errorAlert("bad_source",
                                                         logError.empty() ? "log not open" : logError));
                    return;
                }
                sendControl(client, helloAckMessage(client));
                if (sources_.hasLog()) {
                    sendBinary(client, proto::MsgType::TimelineStatus,
                               proto::encodeTimelineStatus(sources_.replayTimeline(client.replay)));
                    pushSnapshot(client);
                }
                break;
            }
            detachClientLog(client);
            if (msg->source == ctrl::SourceKind::DaqTcp && sources_.kind() == ctrl::SourceKind::DaqTcp) {
                sendControl(client, helloAckMessage(client));
                break;
            }
            std::string selectError;
            pcan::Config pcanCfg;
            pcanCfg.channel = msg->channel;
            pcanCfg.baud = msg->baud;
            pcanCfg.listenOnly = msg->listenOnly;
            if (!applySelectSource(msg->source, msg->host, msg->port, msg->path, selectError, pcanCfg)) {
                const char* code = "bad_source";
                if (msg->source == ctrl::SourceKind::Pcan) {
                    code = selectError == pcan::kUnavailableMessage ? "pcan_unavailable" : "bad_source";
                }
                sendControl(client, ctrl::errorAlert(code, selectError));
                return;
            }
            sendControl(client, helloAckMessage(client));
            break;
        }
        case ctrl::Type::SubscribeSignals:
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            for (const auto id : msg->signalIds) {
                client.subscribed.insert(id);
            }
            refreshDecodeWanted();
            pushSnapshot(client);
            break;
        case ctrl::Type::UnsubscribeSignals:
            for (const auto id : msg->signalIds) {
                client.subscribed.erase(id);
            }
            refreshDecodeWanted();
            break;
        case ctrl::Type::SubscribeCan:
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            client.canMonitor = true;
            break;
        case ctrl::Type::UnsubscribeCan:
            client.canMonitor = false;
            break;
        case ctrl::Type::TimelineControl:
            std::fprintf(stdout, "[photon-backend] timeline_control client=%llu mode=%s cursor=%llu speed=%.3f\n",
                         static_cast<unsigned long long>(client.id),
                         std::string(ctrl::timelineName(msg->timelineMode)).c_str(),
                         static_cast<unsigned long long>(msg->cursorMs), msg->speed);
            if (client.logPointer) {
                if (msg->timelineMode == ctrl::TimelineMode::Live) {
                    detachClientLog(client);
                    sendControl(client, helloAckMessage(client));
                } else {
                    sources_.controlReplay(client.replay, msg->timelineMode, msg->cursorMs, msg->speed);
                }
            } else {
                sources_.controlTimeline(msg->timelineMode, msg->cursorMs, msg->speed);
            }
            break;
        case ctrl::Type::SelectDbc: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            std::string dbcError;
            bool ok = false;
            if (!msg->path.empty()) {
                ok = loadDbcFile(msg->path, dbcError);
            } else if (!msg->dbcId.empty()) {
                ok = loadBundledDbc(msg->dbcId, dbcError);
            } else {
                dbcError = "select_dbc requires path or id";
            }
            if (!ok) {
                sendControl(client, ctrl::errorAlert("bad_dbc", dbcError));
                return;
            }
            std::fprintf(stdout, "[photon-backend] dbc %s (%zu messages)\n", dbc_.label.c_str(),
                         dbc_.messages.size());
            broadcastCatalog();
            break;
        }
        case ctrl::Type::TxArmRequest:
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            if (msg->armed) {
                client.txArmed = true;
                client.lastHeartbeatMs = nowMs;
                sendControl(client, ctrl::txArmStatus(true, "armed"));
                std::fprintf(stdout, "[photon-backend] TX armed client=%s\n", client.label.c_str());
            } else {
                disarm(client, "client_request");
            }
            break;
        case ctrl::Type::TxCanFrame: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            if (sources_.kind() != ctrl::SourceKind::Pcan) {
                sendControl(client, ctrl::errorAlert("tx_refused", "source is not pcan"));
                return;
            }
            if (sources_.listenOnly()) {
                sendControl(client, ctrl::errorAlert("tx_refused", "listen_only is enabled"));
                return;
            }
            if (!client.txArmed) {
                sendControl(client, ctrl::errorAlert("tx_refused", "TX is not armed"));
                return;
            }
            if (!dbcLoaded_) {
                sendControl(client, ctrl::errorAlert("tx_refused", "DBC is not loaded"));
                return;
            }
            std::vector<dbc::SignalValue> values;
            values.reserve(msg->txSignals.size());
            for (const auto& sig : msg->txSignals) {
                values.push_back(dbc::SignalValue{sig.name, sig.value});
            }
            dbc::EncodedFrame frame;
            std::string encError;
            if (!dbc::encodeFrame(dbc_, msg->message, values, frame, &encError)) {
                sendControl(client, ctrl::errorAlert("tx_refused", encError.empty() ? "DBC encode failed" : encError));
                return;
            }
            std::string writeError;
            if (!sources_.writeCan(frame, writeError)) {
                sendControl(client, ctrl::errorAlert("tx_refused", writeError));
                return;
            }
            std::fprintf(stdout, "[photon-backend] tx_can_frame %s id=0x%X dlc=%u client=%s\n",
                         msg->message.c_str(), static_cast<unsigned>(frame.canId),
                         static_cast<unsigned>(frame.dlc), client.label.c_str());
            break;
        }
        case ctrl::Type::Heartbeat:
            break;
        case ctrl::Type::TrackSimList:
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            {
                ctrl::Message listed = ctrl::trackSimListResult(catalog_.list());
                listed.measured = sources_.measuredDrive(dbcLoaded_ ? &dbc_ : nullptr);
                sendControl(client, listed);
            }
            break;
        case ctrl::Type::TrackSimRequest:
            enqueueTrackSim(client, *msg);
            break;
        case ctrl::Type::ArraySimRequest:
            enqueueArraySim(client, *msg);
            break;
        case ctrl::Type::VideoSubscribe: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            // Live pictures are UDP :6800. Do not open live TCP :6700.
            client.videoSubscribed = true;
            videoRelay_.subscribe(client.id, msg->videoMode, msg->startMs, {}, 0);
            std::fprintf(stdout, "[photon-backend] video_subscribe %s client=%s start=%llu\n",
                         std::string(ctrl::videoModeName(msg->videoMode)).c_str(), client.label.c_str(),
                         static_cast<unsigned long long>(msg->startMs));
            std::fflush(stdout);
            streamVideoStatus(nowMs, true);
            break;
        }
        case ctrl::Type::VideoStop:
            videoRelay_.unsubscribe(client.id);
            streamVideoStatus(nowMs, true);
            client.videoSubscribed = false;
            // UI reset (goLive) drops its ring. Forget the seed GOP so the
            // next t=0 locate sends pictures again instead of "idle skip".
            client.videoGopLive = false;
            client.videoSentSeq = 0;
            client.videoGopSent = 0;
            client.videoCache.clear();
            break;
        case ctrl::Type::VideoLocate: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            if (msg->startMs == 0) {
                client.videoSubscribed = true;
                videoRelay_.subscribe(client.id, ctrl::VideoMode::Live, 0, {}, 0);
            }
            videoRelay_.pauseLiveForRpc();
            if (msg->startMs != 0) {
                // The live t=0 poll runs 2.5x a second; the cache line below
                // reports it when it actually ships pictures.
                std::fprintf(stdout, "[photon-backend] video_locate client=%s at=%llu\n", client.label.c_str(),
                             static_cast<unsigned long long>(msg->startMs));
                std::fflush(stdout);
            }
            // Catalog GOP pointers live on MACBOOK. Do not open laptop→:6700
            // (that path is often down and is not the pointer cache).
            if (historyRemote_.enabled()) {
                historyRemote_.requestVideoLocate(client.id, *msg);
                return;
            }
            auto sendCache = [&]() {
                // Only what this client has not been sent yet. The live poll
                // re-locates the same growing GOP, and re-sending it whole put
                // roughly 2x the camera bitrate on the wire for pictures the
                // UI had already decoded.
                std::size_t sent = 0;
                for (const auto& nal : client.videoCache.nals()) {
                    if (nal.seq <= client.videoSentSeq) {
                        continue;
                    }
                    proto::VideoFrame vf;
                    vf.timestampMs = nal.videoMs;
                    vf.flags = nal.flags;
                    vf.nal.assign(nal.nal.begin(), nal.nal.end());
                    sendBinary(client, proto::MsgType::VideoFrame, proto::encodeVideoFrame(vf));
                    client.videoSentSeq = nal.seq;
                    ++sent;
                }
                const auto st = ctrl::videoStatus(!client.videoCache.nals().empty(), videoRelay_.host(),
                                                  videoRelay_.port(), ctrl::VideoMode::Live,
                                                  client.videoCache.newestMs(), client.videoCache.size(), 0);
                sendControl(client, st);
                return sent;
            };
            auto grabIntoCache = [&]() -> bool {
                std::vector<video::ServerFrame> frames;
                std::string grabError;
                if (!videoRelay_.grabLatest(frames, &grabError) || frames.empty()) {
                    return false;
                }
                client.videoCache.clear();
                client.videoSentSeq = 0;
                client.videoGopLive = false;
                for (const auto& frame : frames) {
                    client.videoCache.push(frame.timestampMs, frame.flags, frame.payload);
                }
                sendCache();
                std::fprintf(stdout, "[photon-backend] video grab cache client=%s nals=%zu bytes=%zu\n",
                             client.label.c_str(), client.videoCache.size(), client.videoCache.bytes());
                std::fflush(stdout);
                return true;
            };
            std::vector<std::uint8_t> body;
            std::string locError;
            const auto locateT0 = std::chrono::steady_clock::now();
            if (!videoRelay_.locate(msg->startMs, msg->videoStreamId, msg->videoPrefetchGops, body, &locError)) {
                std::fprintf(stdout, "[photon-backend] video_locate failed client=%s %s\n", client.label.c_str(),
                             locError.empty() ? "locate failed" : locError.c_str());
                std::fflush(stdout);
                sendControl(client, ctrl::errorAlert("video_locate", locError.empty() ? "locate failed" : locError));
                return;
            }
            vidx::LocateReply reply;
            if (!vidx::decodeLocateReply(body, reply)) {
                std::fprintf(stdout, "[photon-backend] video_locate decode failed client=%s bytes=%zu\n",
                             client.label.c_str(), body.size());
                std::fflush(stdout);
                sendControl(client, ctrl::errorAlert("video_locate", "malformed pointer reply"));
                return;
            }
            const std::size_t covIn = reply.coverage.size();
            vidx::clipLocateCoverage(reply, msg->startMs);
            if (reply.coverage.size() != covIn || body.size() > 4096) {
                body = vidx::encodeLocateReply(reply);
            }
            sendBinary(client, proto::MsgType::VideoLocateReply, body);
            if (reply.rows.empty()) {
                std::fprintf(stdout, "[photon-backend] video_locate no pointer rows client=%s\n",
                             client.label.c_str());
                break;
            }
            // Live t=0: entry GOP only (prefetch rows can be RTCP-future).
            // Historical: every prefetch row, in time order, so a 1-minute pin
            // is resident instead of one GOP (~1 s) per locate.
            const bool livePoll = msg->startMs == 0;
            std::vector<vidx::GopRow> loadRows;
            const vidx::GopRow* liveEntry = nullptr;
            for (const auto& row : reply.rows) {
                if (vidx::rowIsContinuation(row) || row.byteLength == 0) {
                    continue;
                }
                if (livePoll) {
                    if (liveEntry == nullptr) {
                        liveEntry = &row;
                    }
                    continue;
                }
                loadRows.push_back(row);
            }
            if (!livePoll) {
                std::sort(loadRows.begin(), loadRows.end(), [](const vidx::GopRow& a, const vidx::GopRow& b) {
                    if (a.startMs != b.startMs) {
                        return a.startMs < b.startMs;
                    }
                    return a.byteLength > b.byteLength;
                });
                std::vector<vidx::GopRow> uniq;
                uniq.reserve(loadRows.size());
                for (const auto& row : loadRows) {
                    if (!uniq.empty() && uniq.back().startMs == row.startMs &&
                        uniq.back().segmentId == row.segmentId && uniq.back().byteOffset == row.byteOffset) {
                        continue;
                    }
                    if (!uniq.empty() && uniq.back().startMs == row.startMs) {
                        continue;
                    }
                    uniq.push_back(row);
                }
                loadRows.swap(uniq);
            }
            vidx::GopRing loaded;
            loaded.setBudget(vidx::kReplayCacheBudget);
            bool appended = false;
            const vidx::GopRow* best = livePoll ? liveEntry : (loadRows.empty() ? nullptr : &loadRows.front());
            const bool sameGop = livePoll && best != nullptr && !client.videoCache.nals().empty() &&
                                 client.videoGopSegment == best->segmentId &&
                                 client.videoGopOffset == best->byteOffset;
            if (sameGop && best->byteLength <= client.videoGopSent) {
                if (client.videoSentSeq == 0 && !client.videoCache.nals().empty()) {
                    sendCache();
                }
                const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - locateT0)
                                    .count();
                std::fprintf(stdout,
                             "[photon-backend] video_locate idle client=%s dt=%lldms cov=%zu->%zu body=%zu\n",
                             client.label.c_str(), static_cast<long long>(dt), covIn, reply.coverage.size(),
                             body.size());
                std::fflush(stdout);
                break;
            }
            if (sameGop && best->byteLength > client.videoGopSent) {
                // Same GOP as last poll, only longer. Read the tail.
                const std::uint32_t already = client.videoGopSent;
                const std::uint32_t tailLen = best->byteLength - already;
                std::vector<std::uint8_t> tail;
                std::string rangeError;
                if (videoRelay_.readRange(best->segmentId, best->byteOffset + already, tailLen, tail,
                                          &rangeError) &&
                    tail.size() == tailLen) {
                    const std::uint64_t span = best->endMs > best->startMs ? best->endMs - best->startMs : 0;
                    const std::uint64_t tailStartMs =
                        best->byteLength == 0 ? best->startMs
                                              : best->startMs + (span * already) / best->byteLength;
                    std::size_t pushed = 0;
                    video::forEachStampedGopNal(tail, tailStartMs, best->endMs,
                                                [&](std::uint64_t ms, std::uint8_t flags,
                                                    std::span<const std::uint8_t> nal) {
                                                    if (video::nalIsTinyPslice(nal)) {
                                                        return;
                                                    }
                                                    client.videoCache.push(ms, flags, nal);
                                                    ++pushed;
                                                });
                    if (pushed > 0) {
                        client.videoGopSent = best->byteLength;
                        appended = true;
                    }
                }
                if (!appended) {
                    client.videoGopLive = false;
                }
            }
            auto pushGop = [&](const vidx::GopRow& row) -> bool {
                if (loaded.bytes() + row.byteLength > loaded.budget() && !loaded.nals().empty()) {
                    return false;
                }
                std::vector<std::uint8_t> gop;
                std::string rangeError;
                if (!videoRelay_.readRange(row.segmentId, row.byteOffset, row.byteLength, gop, &rangeError)) {
                    return true;
                }
                video::forEachStampedGopNal(gop, row.startMs, row.endMs,
                                            [&](std::uint64_t ms, std::uint8_t flags,
                                                std::span<const std::uint8_t> nal) {
                                                if (video::nalIsTinyPslice(nal)) {
                                                    return;
                                                }
                                                loaded.push(ms, flags, nal);
                                            });
                client.videoGopSegment = row.segmentId;
                client.videoGopOffset = row.byteOffset;
                client.videoGopSent = static_cast<std::uint32_t>(gop.size());
                client.videoGopLive = livePoll;
                return true;
            };
            if (!appended && livePoll && best != nullptr) {
                pushGop(*best);
            } else if (!appended && !livePoll) {
                for (const auto& row : loadRows) {
                    if (!pushGop(row)) {
                        break;
                    }
                }
            }
            if (!loaded.nals().empty()) {
                // A fresh ring restarts its sequence numbers, so the watermark
                // has to restart with it or nothing would be sent.
                client.videoCache = std::move(loaded);
                client.videoSentSeq = 0;
            }
            if (client.videoCache.nals().empty() && grabIntoCache()) {
                break;
            }
            const std::size_t sentNals = sendCache();
            if (!livePoll || sentNals != 0) {
                const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - locateT0)
                                    .count();
                std::fprintf(stdout,
                             "[photon-backend] video_locate cache client=%s dt=%lldms rows=%zu nals=%zu sent=%zu "
                             "body=%zu cov=%zu\n",
                             client.label.c_str(), static_cast<long long>(dt), reply.rows.size(),
                             client.videoCache.size(), sentNals, body.size(), reply.coverage.size());
                std::fflush(stdout);
            }
            break;
        }
        case ctrl::Type::VideoClock: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            std::int64_t applied = 0;
            vidx::ClockProv prov = vidx::ClockProv::Receive;
            std::string clockError;
            if (!videoRelay_.setClockOffset(msg->videoClockOffsetMs, applied, prov, &clockError)) {
                sendControl(client, ctrl::errorAlert("video_clock", clockError.empty() ? "nudge failed" : clockError));
                return;
            }
            if (liveWriter_) {
                liveWriter_->store().setVideoClock(applied, vidx::clockProvName(prov));
            }
            auto ack = ctrl::videoClock(applied);
            ack.reason = vidx::clockProvName(prov);
            sendControl(client, ack);
            break;
        }
        case ctrl::Type::CatalogList:
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            if (historyRemote_.enabled()) {
                historyRemote_.requestCatalog();
            }
            sendControl(client, historyCatalogMessage());
            break;
        case ctrl::Type::HistoryQuery:
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            handleHistoryQuery(client, *msg);
            break;
        case ctrl::Type::AnalysisComputeRequest:
            handleAnalysisCompute(client, *msg);
            break;
        case ctrl::Type::AnalysisComputeCancel:
            handleAnalysisCancel(client, *msg);
            break;
        case ctrl::Type::HistoryCancel:
            cancelHistory(client, msg->queryId);
            break;
        case ctrl::Type::HistoryCredit:
            client.historyCreditBytes += msg->creditBytes;
            client.lastHistoryCreditMs = nowMs;
            if (client.historyUnackedBytes > msg->creditBytes) {
                client.historyUnackedBytes -= msg->creditBytes;
            } else {
                client.historyUnackedBytes = 0;
            }
            if (historyRemote_.enabled()) {
                flushRemoteHistoryBin(client);
            }
            flushHistory(client, nowMs);
            break;
        case ctrl::Type::HelloAck:
        case ctrl::Type::TxArmStatus:
        case ctrl::Type::ErrorAlert:
        case ctrl::Type::TrackSimResult:
        case ctrl::Type::ArraySimResult:
        case ctrl::Type::SignalCatalog:
        case ctrl::Type::VideoStatus:
        case ctrl::Type::HistoryResult:
        case ctrl::Type::AnalysisComputeResult:
        case ctrl::Type::CatalogResult:
            sendControl(client, ctrl::errorAlert("bad_message", "server-bound message type not accepted"));
            break;
    }
}

void SessionHub::checkTxWatchdog(std::uint64_t nowTick) {
    if (nowTick - lastTxCheckMs_ < 250) {
        return;
    }
    lastTxCheckMs_ = nowTick;
    for (auto& client : clients_) {
        if (client->txArmed && nowTick - client->lastHeartbeatMs > kTxWatchdogMs) {
            disarm(*client, "heartbeat_timeout");
        }
    }
}

bool SessionHub::loadDbcFile(const std::string& path, std::string& error) {
    auto db = dbc::loadFile(path, &error);
    if (!db) {
        return false;
    }
    const bool changed = !dbcLoaded_ || dbc_.path != db->path || dbc_.label != db->label;
    dbc_ = std::move(*db);
    dbcLoaded_ = true;
    sources_.setDatabase(&dbc_);
    for (auto& client : clients_) {
        client->replay.decode.setDatabase(&dbc_);
    }
    if (changed) {
        stopLiveWriter();
    }
    return true;
}

bool SessionHub::loadBundledDbc(const std::string& id, std::string& error) {
    if (!safeDbcId(id)) {
        error = "invalid bundled DBC id";
        return false;
    }
    const auto path = (std::filesystem::path(assetsDir_) / "dbc" / (id + ".dbc")).string();
    if (!loadDbcFile(path, error)) {
        return false;
    }
    dbc_.label = id;
    return true;
}

}  // namespace photon
