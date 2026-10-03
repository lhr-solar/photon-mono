#include "photon/session.hpp"

#include "session_internal.hpp"

#include "photon/json.hpp"
#include "photon/video_index.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

namespace photon {
namespace {

bool queryCancelled(const tsdb::Query& q) {
    return q.cancel != nullptr && q.cancel->load(std::memory_order_relaxed);
}

std::size_t encodedBytes(const std::vector<std::vector<std::uint8_t>>& chunks) {
    std::size_t n = 0;
    for (const auto& c : chunks) {
        n += c.size();
    }
    return n;
}

bool floatSignal(const tsdb::SignalInfo* info) {
    return info != nullptr && (info->valueType == dbc::ValueType::Float || info->valueType == dbc::ValueType::Double);
}

// Rollups store the raw CAN bits. For an IEEE signal those bits are not the
// physical value, and their average is not the average speed. first and last
// are real samples. The bit extremes are included because positive floats
// sort in the same order as their bits.
void floatRollup(const tsdb::SignalInfo& info, const tsdb::RollupBucket& b, float& mn, float& mx, float& mean) {
    bool have = false;
    auto take = [&](float v) {
        if (!std::isfinite(v)) {
            return;
        }
        if (!have) {
            mn = mx = v;
            have = true;
            return;
        }
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    };
    const float first = static_cast<float>(tsdb::physical(info, b.first));
    const float last = static_cast<float>(tsdb::physical(info, b.last));
    take(first);
    take(last);
    take(static_cast<float>(tsdb::physical(info, b.min)));
    take(static_cast<float>(tsdb::physical(info, b.max)));
    if (!have) {
        mn = mx = mean = 0.0f;
        return;
    }
    mean = std::isfinite(last) ? last : first;
}

double rollupMeanRaw(const tsdb::RollupBucket& b, bool isSigned) {
    if (b.count == 0) {
        return 0.0;
    }
    if (!isSigned) {
        const double wide = std::ldexp(static_cast<double>(b.sumHi), 64) +
                            static_cast<double>(static_cast<std::uint64_t>(b.sum));
        return wide / static_cast<double>(b.count);
    }
    if (!b.wide) {
        return static_cast<double>(b.sum) / static_cast<double>(b.count);
    }
    const double wide = std::ldexp(static_cast<double>(static_cast<std::int64_t>(b.sumHi)), 64) +
                        static_cast<double>(static_cast<std::uint64_t>(b.sum));
    return wide / static_cast<double>(b.count);
}

bool appendEncodedChunks(HistoryEncodeState& st, std::vector<std::vector<std::uint8_t>>& chunks,
                         std::uint32_t queryId, std::uint64_t maxBytes, const std::atomic<bool>* cancel) {
    if (st.store == nullptr) {
        st.more = false;
        return false;
    }
    constexpr std::size_t kMaxChunkSlots = 2000;
    const auto& result = st.result;
    while (st.seriesIdx < result.series.size()) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            chunks.clear();
            st.more = false;
            return false;
        }
        const auto& s = result.series[st.seriesIdx];
        const tsdb::SignalInfo* info = st.store->signal(s.signalId);
        auto phys = [&](std::int64_t raw) -> float {
            return info != nullptr ? static_cast<float>(tsdb::physical(*info, raw)) : static_cast<float>(raw);
        };
        const bool raw = s.level == 0;
        const std::uint8_t fields =
            raw ? proto::kHistRaw
                : static_cast<std::uint8_t>(proto::kHistMin | proto::kHistMax | proto::kHistMean);
        const std::size_t n = raw ? s.timestamps.size() : s.buckets.size();
        if (n == 0) {
            ++st.seriesIdx;
            st.off = 0;
            continue;
        }
        while (st.off < n) {
            if (encodedBytes(chunks) >= maxBytes && !chunks.empty()) {
                st.more = true;
                return true;
            }
            proto::HistoryChunk chunk;
            chunk.queryId = queryId;
            chunk.signalId = s.signalId;
            chunk.level = static_cast<std::uint8_t>(s.level);
            chunk.fields = fields;
            chunk.chunkSeq = st.chunkSeq++;
            const std::size_t take = std::min(kMaxChunkSlots, n - st.off);
            if (raw) {
                chunk.flags = proto::kHistDense;
                chunk.bucketMs = 0;
                chunk.baseMs = s.timestamps[st.off];
                chunk.slotCount = static_cast<std::uint32_t>(take);
                chunk.valueCount = static_cast<std::uint32_t>(take);
                chunk.timestamps.reserve(take);
                chunk.rawV.reserve(take);
                for (std::size_t i = 0; i < take; ++i) {
                    chunk.timestamps.push_back(static_cast<double>(s.timestamps[st.off + i]));
                    chunk.rawV.push_back(phys(s.raw[st.off + i]));
                }
            } else {
                chunk.bucketMs = s.bucketMs;
                chunk.baseMs = s.buckets[st.off].startMs;
                chunk.slotCount = static_cast<std::uint32_t>(take);
                bool dense = true;
                for (std::size_t i = 0; i < take; ++i) {
                    const auto& b = s.buckets[st.off + i];
                    if (!b.present || b.count == 0) {
                        dense = false;
                        break;
                    }
                }
                if (dense) {
                    chunk.flags = proto::kHistDense;
                } else {
                    const std::size_t bytes = (take + 7) / 8;
                    chunk.presence.assign(bytes, 0);
                    for (std::size_t i = 0; i < take; ++i) {
                        const auto& b = s.buckets[st.off + i];
                        if (b.present && b.count > 0) {
                            chunk.presence[i / 8] =
                                static_cast<std::uint8_t>(chunk.presence[i / 8] | (1u << (i % 8)));
                        }
                    }
                }
                for (std::size_t i = 0; i < take; ++i) {
                    const auto& b = s.buckets[st.off + i];
                    if (!b.present || b.count == 0) {
                        continue;
                    }
                    float mn = 0.0f;
                    float mx = 0.0f;
                    float mean = 0.0f;
                    if (floatSignal(info)) {
                        floatRollup(*info, b, mn, mx, mean);
                    } else {
                        mn = phys(b.min);
                        mx = phys(b.max);
                        if (info != nullptr && info->factor < 0.0) {
                            std::swap(mn, mx);
                        }
                        const double meanRaw = rollupMeanRaw(b, info == nullptr || info->isSigned);
                        mean = info != nullptr ? static_cast<float>(meanRaw * info->factor + info->offset)
                                               : static_cast<float>(meanRaw);
                    }
                    chunk.minV.push_back(mn);
                    chunk.maxV.push_back(mx);
                    chunk.meanV.push_back(mean);
                }
                chunk.valueCount = static_cast<std::uint32_t>(chunk.minV.size());
            }
            st.off += take;
            const bool last = st.seriesIdx + 1 >= result.series.size() && st.off >= n;
            if (last) {
                chunk.flags = static_cast<std::uint16_t>(chunk.flags | proto::kHistFinal);
            }
            chunks.push_back(proto::encodeHistoryChunk(chunk));
        }
        ++st.seriesIdx;
        st.off = 0;
    }
    st.more = false;
    return false;
}

HistoryWork buildHistoryWork(std::shared_ptr<tsdb::Store> store, const tsdb::Query& q, std::uint32_t queryId,
                             std::uint64_t maxBytes) {
    HistoryWork work;
    work.queryId = queryId;
    if (store == nullptr || queryCancelled(q)) {
        return work;
    }
    work.encode.store = std::move(store);
    work.encode.result = work.encode.store->query(q);
    if (queryCancelled(q)) {
        return work;
    }

    std::vector<ctrl::Message::HistorySeries> series;
    series.reserve(work.encode.result.series.size());
    constexpr std::size_t kMaxChunkSlots = 2000;
    for (const auto& s : work.encode.result.series) {
        ctrl::Message::HistorySeries row;
        row.signalId = s.signalId;
        row.level = s.level;
        row.bucketMs = s.bucketMs;
        const tsdb::SignalInfo* info = work.encode.store->signal(s.signalId);
        auto phys = [&](std::int64_t raw) -> float {
            return info != nullptr ? static_cast<float>(tsdb::physical(*info, raw)) : static_cast<float>(raw);
        };
        if (s.prior) {
            row.prior.present = true;
            row.prior.t = s.prior->timestampMs;
            row.prior.v = phys(s.prior->raw);
        }
        if (s.next) {
            row.next.present = true;
            row.next.t = s.next->timestampMs;
            row.next.v = phys(s.next->raw);
        }
        const bool raw = s.level == 0;
        if (raw) {
            row.points = static_cast<std::uint32_t>(s.timestamps.size());
            if (!s.timestamps.empty()) {
                row.tMin = s.timestamps.front();
                row.tMax = s.timestamps.back();
            }
        } else {
            std::uint32_t present = 0;
            for (const auto& b : s.buckets) {
                if (b.present && b.count > 0) {
                    ++present;
                    if (row.tMin == 0 || b.startMs < row.tMin) {
                        row.tMin = b.startMs;
                    }
                    row.tMax = b.startMs + s.bucketMs;
                }
            }
            row.points = present;
        }
        const std::size_t n = raw ? s.timestamps.size() : s.buckets.size();
        row.chunks = row.points == 0 ? 0 : static_cast<std::uint32_t>((n + kMaxChunkSlots - 1) / kMaxChunkSlots);
        series.push_back(std::move(row));
    }
    const auto cap = maxBytes == 0 ? 4 * 1024 * 1024 : maxBytes;
    appendEncodedChunks(work.encode, work.chunks, queryId, cap, q.cancel);
    work.result = ctrl::historyResult(queryId, work.encode.result.level, work.encode.result.bucketMs,
                                      work.encode.result.fromMs, work.encode.result.toMs, std::move(series),
                                      work.encode.result.truncated || work.encode.more);
    return work;
}

}  // namespace

ctrl::Message SessionHub::catalogMessage() const {
    std::vector<ctrl::CatalogSignal> entries;
    if (dbcLoaded_) {
        for (const auto& message : dbc_.messages) {
            for (const auto& sig : message.signals) {
                ctrl::CatalogSignal row;
                row.id = sig.signalId;
                row.messageId = message.canId;
                row.message = sig.message.empty() ? message.name : sig.message;
                row.name = sig.name;
                row.unit = sig.unit;
                row.min = sig.min;
                row.max = sig.max;
                entries.push_back(std::move(row));
            }
        }
    }
    auto msg = ctrl::signalCatalog(std::move(entries));
    if (dbcLoaded_) {
        msg.dbcId = dbc_.label;
        msg.dbcPath = dbc_.path;
    }
    return msg;
}

ctrl::Message SessionHub::historyCatalogMessage() const {
    if (historyRemote_.enabled()) {
        return historyRemote_.catalogReady() ? historyRemote_.catalogMessage() : ctrl::catalogResult({});
    }
    std::vector<ctrl::Message::CatalogSession> rows;
    if (!tsdbDir_.empty()) {
        const auto listed = tsdb::listCatalog(tsdbDir_);
        rows.reserve(listed.size());
        for (const auto& item : listed) {
            ctrl::Message::CatalogSession row;
            row.id = item.id;
            row.path = item.path;
            row.minMs = item.minMs;
            row.maxMs = item.maxMs;
            row.dbcHash = item.dbcSha;
            row.sampleCount = item.sampleCount;
            row.hasVideo = item.hasVideo;
            rows.push_back(std::move(row));
        }
    }
    return ctrl::catalogResult(std::move(rows));
}

void SessionHub::sendCatalog(ClientSession& client) {
    sendControl(client, catalogMessage());
}

void SessionHub::broadcastCatalog() {
    const auto msg = catalogMessage();
    for (auto& client : clients_) {
        if (client->helloOk) {
            sendControl(*client, msg);
        }
    }
}

std::shared_ptr<tsdb::Store> SessionHub::openHistoryStore(const std::string& sessionId, std::uint64_t atMs,
                                                          std::string* error) {
    std::string id = sessionId;
    const auto root = std::filesystem::path(tsdbDir_);
    auto loadCatalog = [&]() -> std::optional<json::Value> {
        std::ifstream in(root / "catalog.json", std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return json::parse(text);
    };
    if (id.empty() || id == "default") {
        if (const auto cat = loadCatalog(); cat && (*cat)["sessions"].isArray()) {
            const auto& arr = (*cat)["sessions"].asArray();
            for (const auto& row : arr) {
                const auto minMs = static_cast<std::uint64_t>(row["min_ms"].asInt());
                const auto maxMs = static_cast<std::uint64_t>(row["max_ms"].asInt());
                if (atMs != 0 && atMs >= minMs && atMs <= maxMs) {
                    id = row["id"].asString();
                    break;
                }
            }
            if ((id.empty() || id == "default") && !arr.empty()) {
                id = arr[0]["id"].asString();
            }
        }
    }
    if (id.empty()) {
        if (error) {
            *error = "no TSDB session";
        }
        return nullptr;
    }
    auto it = historyStores_.find(id);
    if (it != historyStores_.end()) {
        it->second->refresh(nullptr);
        return it->second;
    }
    auto store = std::make_shared<tsdb::Store>();
    if (!store->open(root / id, error)) {
        return nullptr;
    }
    historyStores_.emplace(id, store);
    return store;
}

void SessionHub::flushRemoteHistoryBin(ClientSession& client) {
    while (!client.remoteHistoryBin.empty()) {
        const auto& payload = client.remoteHistoryBin.front();
        const auto need = payload.size();
        if (need > client.historyCreditBytes) {
            break;
        }
        const auto before = client.conn.outboundBytes();
        client.conn.queueBinary(payload);
        noteQueued(client, true, before);
        client.historyCreditBytes -= need;
        client.historyUnackedBytes += need;
        historyRemote_.sendCredit(0, need);
        client.remoteHistoryBin.pop_front();
    }
}

void SessionHub::flushHistoryRemote() {
    for (auto& delivery : historyRemote_.takeDeliveries()) {
        if (delivery.text && delivery.text->type == ctrl::Type::CatalogResult) {
            for (auto& client : clients_) {
                if (client->helloOk) {
                    sendControl(*client, *delivery.text);
                }
            }
            continue;
        }
        const auto frame = delivery.binary.empty() ? std::nullopt : proto::decodeFrame(delivery.binary);
        const bool videoBin =
            frame && (frame->header.msgType == static_cast<std::uint8_t>(proto::MsgType::VideoFrame) ||
                      frame->header.msgType == static_cast<std::uint8_t>(proto::MsgType::VideoLocateReply));
        const bool videoText = delivery.text && (delivery.text->type == ctrl::Type::VideoStatus ||
                                                 delivery.text->type == ctrl::Type::ErrorAlert);
        if (videoBin || videoText) {
            for (auto& slot : clients_) {
                if (!slot->helloOk) {
                    continue;
                }
                if (delivery.clientId != 0 && slot->id != delivery.clientId) {
                    continue;
                }
                if (delivery.text) {
                    sendControl(*slot, *delivery.text);
                }
                if (videoBin) {
                    const auto before = slot->conn.outboundBytes();
                    slot->conn.queueBinary(delivery.binary);
                    noteQueued(*slot, false, before);
                    if (frame->header.msgType == static_cast<std::uint8_t>(proto::MsgType::VideoFrame)) {
                        if (const auto vf = proto::decodeVideoFrame(frame->payload)) {
                            slot->videoCache.push(vf->timestampMs, vf->flags, vf->nal);
                        }
                    } else {
                        vidx::LocateReply reply;
                        if (vidx::decodeLocateReply(frame->payload, reply)) {
                            std::fprintf(stdout,
                                         "[photon-backend] history-remote video_locate rows=%zu cache nals=%zu\n",
                                         reply.rows.size(), slot->videoCache.size());
                            std::fflush(stdout);
                            const auto st = ctrl::videoStatus(!reply.rows.empty() || !slot->videoCache.nals().empty(),
                                                              videoRelay_.host(), videoRelay_.port(),
                                                              ctrl::VideoMode::Live, slot->videoCache.newestMs(),
                                                              slot->videoCache.size(), 0);
                            sendControl(*slot, st);
                        }
                    }
                }
            }
            continue;
        }
        ClientSession* client = delivery.clientId == 0 ? nullptr : findClient(delivery.clientId);
        if (!client) {
            continue;
        }
        if (delivery.text) {
            sendControl(*client, *delivery.text);
        }
        if (!delivery.binary.empty()) {
            client->remoteHistoryBin.push_back(std::move(delivery.binary));
            flushRemoteHistoryBin(*client);
        }
    }
}

void SessionHub::cancelHistory(ClientSession& client, std::uint32_t queryId) {
    if (historyRemote_.enabled()) {
        historyRemote_.cancelHistory(client.id, queryId);
    }
    auto cancelSlot = [&](HistoryQuerySlot& slot) {
        slot.cancelled = true;
        if (slot.cancel) {
            slot.cancel->store(true);
        }
        slot.chunks.clear();
    };
    if (queryId == 0) {
        for (auto& [id, slot] : client.historyQueries) {
            (void)id;
            cancelSlot(slot);
        }
        for (auto& job : historyJobs_) {
            if (job.clientId == client.id && job.cancel) {
                job.cancel->store(true);
            }
        }
        return;
    }
    if (auto it = client.historyQueries.find(queryId); it != client.historyQueries.end()) {
        cancelSlot(it->second);
    }
    for (auto& job : historyJobs_) {
        if (job.clientId == client.id && job.queryId == queryId && job.cancel) {
            job.cancel->store(true);
        }
    }
}

void SessionHub::handleHistoryQuery(ClientSession& client, const ctrl::Message& msg) {
    if (msg.creditBytes != 0) {
        client.historyCreditBytes += msg.creditBytes;
        client.lastHistoryCreditMs = 0;
    }
    if (historyRemote_.enabled()) {
        const std::uint64_t span = msg.toMs >= msg.fromMs ? msg.toMs - msg.fromMs : 0;
        constexpr std::uint64_t kUnixMs2020 = 1577836800000ull;
        constexpr std::uint64_t kMaxHistorySpanMs = 7ull * 24ull * 60ull * 60ull * 1000ull;
        const bool epochBomb = msg.fromMs < kUnixMs2020 && msg.toMs >= kUnixMs2020;
        if (epochBomb || span > kMaxHistorySpanMs) {
            sendControl(client, ctrl::historyResult(msg.queryId, 0, 0, msg.fromMs, msg.toMs, {}, false));
            return;
        }
        historyRemote_.requestHistory(client.id, msg);
        return;
    }
    const std::uint64_t span = msg.toMs >= msg.fromMs ? msg.toMs - msg.fromMs : 0;
    // Plots used to ask for [0, unix-now] when the live timeline min was 0.
    // That is weeks of tiles and aborts the WASM tab. Keep short synthetic
    // windows (tests use from=0,to=2000) and real day/session unix ranges.
    constexpr std::uint64_t kUnixMs2020 = 1577836800000ull;
    constexpr std::uint64_t kMaxHistorySpanMs = 7ull * 24ull * 60ull * 60ull * 1000ull;
    const bool epochBomb = msg.fromMs < kUnixMs2020 && msg.toMs >= kUnixMs2020;
    if (epochBomb || span > kMaxHistorySpanMs) {
        sendControl(client, ctrl::historyResult(msg.queryId, 0, 0, msg.fromMs, msg.toMs, {}, false));
        return;
    }
    std::string error;
    auto store = openHistoryStore(msg.sessionId, msg.fromMs, &error);
    if (store == nullptr) {
        sendControl(client, ctrl::historyResult(msg.queryId, 0, 0, msg.fromMs, msg.toMs, {}, false));
        return;
    }
    tsdb::Query q;
    q.fromMs = msg.fromMs;
    q.toMs = msg.toMs;
    q.signalIds = msg.signalIds;
    q.maxPoints = msg.maxPoints == 0 ? 2000 : msg.maxPoints;
    const std::uint32_t queryId = msg.queryId;
    const std::uint64_t clientId = client.id;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    q.cancel = cancel.get();
    HistoryQuerySlot slot;
    slot.phase = HistoryQuerySlot::Phase::Running;
    slot.cancel = cancel;
    client.historyQueries[queryId] = std::move(slot);
    const auto cap = client.historyCreditBytes == 0
                         ? 256 * 1024
                         : std::min<std::uint64_t>(client.historyCreditBytes, 8ull * 1024 * 1024);
    historyJobs_.push_back(HistoryJob{
        clientId, queryId, cancel, store,
        historyPool_->submit([store, q, queryId, cap]() { return buildHistoryWork(store, q, queryId, cap); }),
    });
}

void SessionHub::pollHistoryJobs() {
    for (auto it = historyJobs_.begin(); it != historyJobs_.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto work = it->future.get();
        if (auto* client = findClient(it->clientId)) {
            auto qit = client->historyQueries.find(work.queryId);
            if (qit != client->historyQueries.end() && !qit->second.cancelled &&
                !(it->cancel && it->cancel->load())) {
                qit->second.phase = HistoryQuerySlot::Phase::Ready;
                qit->second.encode = std::move(work.encode);
                sendControl(*client, work.result);
                for (auto& chunk : work.chunks) {
                    qit->second.chunks.push_back(std::move(chunk));
                }
                flushHistory(*client, 0);
            } else if (qit != client->historyQueries.end()) {
                client->historyQueries.erase(qit);
            }
        }
        it = historyJobs_.erase(it);
    }
}

void SessionHub::flushHistory(ClientSession& client, std::uint64_t nowMs) {
    if (historyRemote_.enabled()) {
        flushRemoteHistoryBin(client);
    }
    constexpr std::uint64_t kCreditTimeoutMs = 8000;
    if (nowMs != 0 && client.lastHistoryCreditMs != 0 && nowMs > client.lastHistoryCreditMs &&
        nowMs - client.lastHistoryCreditMs >= kCreditTimeoutMs && !client.historyQueries.empty()) {
        // A vanished client credit must not leave encoded chunks stranded (§9.10).
        client.historyCreditBytes += 1024 * 1024;
        client.lastHistoryCreditMs = nowMs;
    }
    for (auto& [id, slot] : client.historyQueries) {
        if (slot.phase == HistoryQuerySlot::Phase::Running || slot.cancelled) {
            continue;
        }
        if (slot.encode.more && encodedBytes(slot.chunks) < client.historyCreditBytes) {
            const auto room = client.historyCreditBytes - encodedBytes(slot.chunks);
            const auto cap = std::min<std::uint64_t>(room, 8ull * 1024 * 1024);
            if (cap > 0) {
                appendEncodedChunks(slot.encode, slot.chunks, id, cap, slot.cancel.get());
            }
        }
        while (!slot.chunks.empty()) {
            const auto& payload = slot.chunks.front();
            const auto need = payload.size() + proto::kHeaderSize;
            if (need > client.historyCreditBytes) {
                break;
            }
            sendBinary(client, proto::MsgType::HistoryChunk, payload);
            client.historyCreditBytes -= need;
            client.historyUnackedBytes += need;
            if (nowMs != 0 && client.lastHistoryCreditMs == 0) {
                client.lastHistoryCreditMs = nowMs;
            }
            slot.chunks.erase(slot.chunks.begin());
        }
    }
    for (auto it = client.historyQueries.begin(); it != client.historyQueries.end();) {
        const bool running = it->second.phase == HistoryQuerySlot::Phase::Running && !it->second.cancelled;
        if (!running && it->second.chunks.empty() && !it->second.encode.more) {
            it = client.historyQueries.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace photon
