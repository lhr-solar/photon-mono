#include "photon/session.hpp"

#include <algorithm>

namespace photon {
namespace {

constexpr std::uint64_t kTelemetryPeriodMs = 16;  // ~60 Hz live gauges
constexpr std::uint64_t kCanBurstPeriodMs = 16;
constexpr std::uint64_t kTimelinePeriodMs = 33;
constexpr std::size_t kTelemetryCongestBytes = 256 * 1024;

std::size_t historyResident(const ClientSession& client) {
    std::size_t n = 0;
    for (const auto& tag : client.outboundTags) {
        if (tag.history) {
            n += tag.bytes;
        }
    }
    return n;
}

std::size_t liveQueued(const ClientSession& client) {
    const auto out = client.conn.outboundBytes();
    const auto hist = std::min(out, historyResident(client));
    return out > hist ? out - hist : 0;
}

}  // namespace

void SessionHub::sendBinary(ClientSession& client, proto::MsgType type, std::span<const std::uint8_t> payload) {
    const auto frame = proto::encodeFrame(type, binarySeq_++, payload);
    const auto before = client.conn.outboundBytes();
    client.conn.queueBinary(frame);
    noteQueued(client, type == proto::MsgType::HistoryChunk, before);
}

void SessionHub::refreshDecodeWanted() {
    std::unordered_set<std::uint32_t> liveIds;
    for (const auto& client : clients_) {
        if (!client->helloOk) {
            continue;
        }
        if (client->logPointer) {
            client->replay.decode.setWanted(client->subscribed);
            continue;
        }
        liveIds.insert(client->subscribed.begin(), client->subscribed.end());
    }
    sources_.setDecodeWanted(std::move(liveIds));
}

void SessionHub::streamTelemetry(std::uint64_t nowTick) {
    if (nowTick - lastTelemetryMs_ < kTelemetryPeriodMs) {
        return;
    }
    lastTelemetryMs_ = nowTick;
    std::unordered_set<std::uint32_t> allIds;
    for (const auto& client : clients_) {
        if (client->helloOk) {
            allIds.insert(client->subscribed.begin(), client->subscribed.end());
        }
    }
    const auto liveBlocks = allIds.empty() ? std::vector<proto::TelemetryBlock>{}
                                           : sources_.takeTelemetry(allIds);
    for (auto& client : clients_) {
        if (!client->helloOk || client->subscribed.empty()) {
            continue;
        }
        if (client->logPointer) {
            const auto blocks = sources_.takeReplayTelemetry(client->replay, client->subscribed);
            if (!blocks.empty()) {
                sendTelemetryBlocks(*client, blocks, false);
            }
            continue;
        }
        std::vector<proto::TelemetryBlock> filtered;
        for (const auto& block : liveBlocks) {
            if (client->subscribed.contains(block.signalId)) {
                filtered.push_back(block);
            }
        }
        if (filtered.empty()) {
            continue;
        }
        // Video can sit in the WS buffer well above kTelemetryCongestBytes.
        // Live gauges must still move; only replay/history stays gated.
        sendTelemetryBlocks(*client, filtered, true);
    }
}

void SessionHub::streamCan(std::uint64_t nowTick) {
    if (nowTick - lastCanBurstMs_ < kCanBurstPeriodMs) {
        return;
    }
    lastCanBurstMs_ = nowTick;
    auto liveFrames = sources_.takeCanFrames();
    if (!liveFrames.empty()) {
        feedLive(liveFrames);
        // Keep every live frame so the CAN Rate Hz column matches the bus.
        // latestCanPerId would cap the UI at 1/burst (~10 Hz when this was 100 ms).
    }
    const auto livePayload =
        liveFrames.empty() ? std::vector<std::uint8_t>{} : proto::encodeCanMonitorBatch(liveFrames);
    for (auto& client : clients_) {
        if (!client->helloOk) {
            continue;
        }
        if (client->logPointer) {
            if (!client->canMonitor || liveQueued(*client) > kTelemetryCongestBytes) {
                continue;
            }
            auto frames = proto::latestCanPerId(sources_.takeReplayCan(client->replay));
            if (frames.empty()) {
                continue;
            }
            sendBinary(*client, proto::MsgType::CanMonitorBatch, proto::encodeCanMonitorBatch(frames));
            continue;
        }
        if (!livePayload.empty() && client->canMonitor) {
            sendBinary(*client, proto::MsgType::CanMonitorBatch, livePayload);
        }
    }
}

void SessionHub::streamTimeline(std::uint64_t nowTick) {
    if (nowTick - lastTimelineMs_ < kTimelinePeriodMs) {
        return;
    }
    lastTimelineMs_ = nowTick;
    if (sources_.kind() == ctrl::SourceKind::None) {
        return;
    }
    const auto liveStatus = sources_.timeline();
    const auto livePayload = proto::encodeTimelineStatus(liveStatus);
    for (auto& client : clients_) {
        if (!client->helloOk) {
            continue;
        }
        if (client->logPointer) {
            const auto status = sources_.replayTimeline(client->replay);
            sendBinary(*client, proto::MsgType::TimelineStatus, proto::encodeTimelineStatus(status));
            continue;
        }
        sendBinary(*client, proto::MsgType::TimelineStatus, livePayload);
    }
}

void SessionHub::sendTelemetryBlocks(ClientSession& client, const std::vector<proto::TelemetryBlock>& blocks,
                                     bool ignoreCongest) {
    if (blocks.empty()) {
        return;
    }
    constexpr std::size_t kMaxChunk = 180 * 1024;
    std::vector<proto::TelemetryBlock> chunk;
    std::size_t bytes = 0;
    auto flush = [&]() {
        if (chunk.empty()) {
            return;
        }
        if (!ignoreCongest && liveQueued(client) > kTelemetryCongestBytes) {
            chunk.clear();
            bytes = 0;
            return;
        }
        const auto payload = proto::encodeTelemetryBatch(chunk);
        sendBinary(client, proto::MsgType::TelemetryBatch, payload);
        chunk.clear();
        bytes = 0;
    };
    for (const auto& block : blocks) {
        const std::size_t add = 8 + std::min(block.timestamps.size(), block.values.size()) * 16;
        if (!chunk.empty() && bytes + add > kMaxChunk) {
            flush();
        }
        chunk.push_back(block);
        bytes += add;
    }
    flush();
}

void SessionHub::pushSnapshot(ClientSession& client) {
    if (!client.helloOk || client.subscribed.empty()) {
        return;
    }
    const auto blocks = client.logPointer ? sources_.snapshotReplay(client.replay, client.subscribed)
                                          : sources_.snapshotTelemetry(client.subscribed);
    if (blocks.empty()) {
        return;
    }
    // Latest first so a second window's gauges match before the lookback lands.
    std::vector<proto::TelemetryBlock> tips;
    tips.reserve(blocks.size());
    for (const auto& block : blocks) {
        if (block.timestamps.empty() || block.values.empty()) {
            continue;
        }
        proto::TelemetryBlock tip;
        tip.signalId = block.signalId;
        tip.timestamps.push_back(block.timestamps.back());
        tip.values.push_back(block.values.back());
        tips.push_back(std::move(tip));
    }
    sendTelemetryBlocks(client, tips, true);
    sendTelemetryBlocks(client, blocks, true);
}

void SessionHub::bindClientReplay(ClientSession& client) {
    if (!client.logPointer || !sources_.hasLog()) {
        return;
    }
    if (dbcLoaded_ && client.replay.decode.database() != &dbc_) {
        client.replay.decode.setDatabase(&dbc_);
    }
    sources_.attachReplay(client.replay);
}

void SessionHub::resetClientReplays() {
    for (auto& client : clients_) {
        sources_.resetReplay(client->replay);
        if (client->helloOk) {
            bindClientReplay(*client);
        }
    }
}

void SessionHub::tickClientReplays(std::uint64_t nowMs) {
    if (!sources_.hasLog()) {
        return;
    }
    for (auto& client : clients_) {
        if (!client->helloOk || !client->logPointer) {
            continue;
        }
        if (!client->replay.bound) {
            bindClientReplay(*client);
        }
        sources_.tickReplay(client->replay, nowMs);
    }
}

void SessionHub::broadcastSnapshots() {
    for (auto& client : clients_) {
        if (client->helloOk) {
            pushSnapshot(*client);
        }
    }
}

}  // namespace photon
