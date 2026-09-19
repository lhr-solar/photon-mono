#include "photon/session.hpp"

#include "photon/analysis_compute.hpp"
#include "photon/csv_export.hpp"
#include "photon/json.hpp"
#include "photon/log_sessions.hpp"
#include "photon/video_index.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>

namespace photon {
namespace {

constexpr std::uint64_t kTelemetryPeriodMs = 16;  // ~60 Hz live gauges
constexpr std::uint64_t kCanBurstPeriodMs = 16;
constexpr std::uint64_t kTimelinePeriodMs = 33;
constexpr std::uint64_t kTxWatchdogMs = 3000;
constexpr std::size_t kVideoCongestBytes = 512 * 1024;
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
                    float mn = phys(b.min);
                    float mx = phys(b.max);
                    if (info != nullptr && info->factor < 0.0) {
                        std::swap(mn, mx);
                    }
                    chunk.minV.push_back(mn);
                    chunk.maxV.push_back(mx);
                    const double meanRaw = rollupMeanRaw(b, info == nullptr || info->isSigned);
                    chunk.meanV.push_back(info != nullptr
                                              ? static_cast<float>(meanRaw * info->factor + info->offset)
                                              : static_cast<float>(meanRaw));
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

std::uint64_t nowMs() {
    using clock = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

std::string sidecarClientIdFor(const ClientSession& client) {
    std::string id = std::to_string(client.id) + ":" + client.label;
    if (id.size() > 96) {
        id.resize(96);
    }
    for (char& c : id) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '_' || c == '-' || c == '.' || c == ':') {
            continue;
        }
        c = '_';
    }
    return id;
}

}  // namespace

class SessionHub::HistoryPool {
public:
    explicit HistoryPool(std::size_t workers = 4) {
        threads_.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            threads_.emplace_back([this] { loop(); });
        }
    }

    ~HistoryPool() { stop(); }

    std::future<HistoryWork> submit(std::function<HistoryWork()> fn) {
        std::packaged_task<HistoryWork()> task(std::move(fn));
        auto fut = task.get_future();
        {
            std::lock_guard<std::mutex> g(mu_);
            if (stop_) {
                return fut;
            }
            q_.push_back(std::move(task));
        }
        cv_.notify_one();
        return fut;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (stop_) {
                return;
            }
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }

private:
    void loop() {
        for (;;) {
            std::packaged_task<HistoryWork()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) {
                    return;
                }
                task = std::move(q_.front());
                q_.pop_front();
            }
            task();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::packaged_task<HistoryWork()>> q_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

SessionHub::SessionHub(std::uint16_t wsPort, std::string daqHost, std::uint16_t daqPort, std::string assetsDir,
                       std::string videoHost, std::uint16_t videoPort, ctrl::SourceKind startupSource,
                       std::string startupLogPath, std::string tsdbDir, std::uint16_t videoUdpPort)
    : wsPort_(wsPort)
    , daqHost_(std::move(daqHost))
    , daqPort_(daqPort)
    , sources_(daqHost_, daqPort_)
    , videoRelay_(videoHost.empty() ? daqHost_ : std::move(videoHost), videoPort == 0 ? 6700 : videoPort,
                  videoUdpPort)
    , catalog_(TrackCatalog::tracksDirFromAssets(assetsDir.empty() ? TrackCatalog::discoverAssetsDir() : assetsDir))
    , assetsDir_(assetsDir.empty() ? TrackCatalog::discoverAssetsDir() : std::move(assetsDir))
    , startupSource_(startupSource)
    , startupLogPath_(std::move(startupLogPath))
    , tsdbDir_(std::move(tsdbDir)) {
    if (tsdbDir_.empty()) {
        if (const char* env =
#if defined(_MSC_VER)
                []() -> const char* {
                    static char buf[512];
                    size_t n = 0;
                    return getenv_s(&n, buf, sizeof(buf), "PHOTON_TSDB") == 0 && n > 1 ? buf : nullptr;
                }()
#else
                std::getenv("PHOTON_TSDB")
#endif
        ) {
            tsdbDir_ = env;
        } else {
            tsdbDir_ = "data/tsdb";
        }
    }
    historyPool_ = std::make_unique<HistoryPool>();
}

SessionHub::~SessionHub() {
    stop();
}

void SessionHub::setClientPassword(std::string password) {
    sources_.setClientPassword(password);
    videoRelay_.setPassword(std::move(password));
}

void SessionHub::setAnalysisCompute(std::string addr) {
    analysisComputeAddr_ = std::move(addr);
}

void SessionHub::setHistoryRemote(std::string host, std::uint16_t port, std::string path, std::string password) {
    historyRemote_.setTarget(std::move(host), port, std::move(path), std::move(password));
    if (historyRemote_.enabled()) {
        std::fprintf(stdout, "[photon-backend] history-remote enabled\n");
    }
}

bool SessionHub::start() {
    listener_ = net::listenLoopback(wsPort_);
    if (!listener_.valid()) {
        std::fprintf(stderr, "[photon-backend] failed to bind 127.0.0.1:%u\n", static_cast<unsigned>(wsPort_));
        return false;
    }
    std::fprintf(stdout, "[photon-backend] websocket listening on ws://127.0.0.1:%u\n",
                 static_cast<unsigned>(listener_.localPort() != 0 ? listener_.localPort() : wsPort_));
    std::fprintf(stdout, "[photon-backend] daq_tcp default %s:%u\n", daqHost_.c_str(), static_cast<unsigned>(daqPort_));
    std::fprintf(stdout, "[photon-backend] video default %s:%u\n", videoRelay_.host().c_str(),
                 static_cast<unsigned>(videoRelay_.port()));
    std::fprintf(stdout, "[photon-backend] track_sim catalog %s\n", catalog_.tracksDir().c_str());
    if (analysisComputeAddr_.empty()) {
        std::fprintf(stdout, "[photon-backend] analysis-compute off\n");
    } else {
        std::fprintf(stdout, "[photon-backend] analysis-compute %s\n", analysisComputeAddr_.c_str());
    }
    std::string dbcError;
    if (loadBundledDbc("carcan_telemcan", dbcError)) {
        std::fprintf(stdout, "[photon-backend] dbc bundled carcan_telemcan (%zu messages)\n",
                     dbc_.messages.size());
    } else {
        std::fprintf(stdout, "[photon-backend] dbc not loaded (%s)\n", dbcError.c_str());
    }
    std::string selectError;
    refreshDriveDays();
    if (!applySelectSource(startupSource_, daqHost_, daqPort_, startupLogPath_, selectError)) {
        std::fprintf(stderr, "[photon-backend] failed to activate --source %s: %s\n",
                     std::string(ctrl::sourceName(startupSource_)).c_str(), selectError.c_str());
        return false;
    }
    recoverCrashedLive();
    sources_.setDecodeWanted({});
    std::fprintf(stdout, "[photon-backend] waiting for UI hello\n");
    return true;
}

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

bool SessionHub::attachClientLog(ClientSession& client, const std::string& path, std::string& error) {
    const std::string resolved = logsess::resolveReplayPath(path);
    const bool replacing = !sources_.hasLog() || sources_.logPath() != resolved;
    if (replacing) {
        // ReplayCursor holds a pointer into LogReplaySource::archive_. Replacing
        // the log without unbinding every view is a use-after-free on the next
        // tickClientReplays (same tick as this select_source).
        for (auto& c : clients_) {
            sources_.resetReplay(c->replay);
        }
    }
    if (!sources_.openLog(path, error)) {
        if (replacing) {
            for (auto& c : clients_) {
                if (c->helloOk && c->logPointer) {
                    bindClientReplay(*c);
                }
            }
        }
        return false;
    }
    client.logPointer = true;
    if (!replacing) {
        bindClientReplay(client);
        refreshDecodeWanted();
        return true;
    }
    for (auto& c : clients_) {
        if (!c->logPointer) {
            continue;
        }
        bindClientReplay(*c);
        if (c.get() != &client && c->helloOk) {
            sendControl(*c, helloAckMessage(*c));
        }
    }
    refreshDecodeWanted();
    return true;
}

void SessionHub::detachClientLog(ClientSession& client) {
    client.logPointer = false;
    sources_.resetReplay(client.replay);
    refreshDecodeWanted();
    if (client.helloOk) {
        if (sources_.kind() != ctrl::SourceKind::None) {
            sendBinary(client, proto::MsgType::TimelineStatus, proto::encodeTimelineStatus(sources_.timeline()));
        }
        pushSnapshot(client);
    }
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

void SessionHub::stop() {
    cancelAllAnalysisJobs();
    for (auto& job : historyJobs_) {
        if (job.cancel) {
            job.cancel->store(true);
        }
    }
    for (auto& job : historyJobs_) {
        if (job.future.valid()) {
            job.future.wait();
        }
    }
    historyJobs_.clear();
    historyStores_.clear();
    stopLiveWriter();
    for (auto& client : clients_) {
        if (client->txArmed) {
            disarm(*client, "server_stop");
        }
        client->conn.queueClose(1001, "server stop");
        client->conn.flushOutbound();
    }
    clients_.clear();
    for (auto& job : trackSimJobs_) {
        if (job.future.valid()) {
            job.future.wait();
        }
    }
    trackSimJobs_.clear();
    cancelAllAnalysisJobs();
    sources_.stop();
    videoRelay_.stop();
    listener_.close();
    if (historyPool_) {
        historyPool_->stop();
    }
    for (int i = 0; i < 200 && exportBusy_.load() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void SessionHub::startCsvExport(net::Socket sock, std::string target) {
    if (exportBusy_.load() >= 2) {
        sock.setNonBlocking(false);
        const char* resp =
            "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: 12\r\nConnection: close\r\n\r\nexport busy\n";
        sock.sendSome(resp, std::strlen(resp));
        return;
    }
    exportBusy_.fetch_add(1);
    std::thread([sock = std::move(sock), target = std::move(target), dir = tsdbDir_,
                 busy = &exportBusy_]() mutable {
        handleCsvExport(std::move(sock), target, dir);
        busy->fetch_sub(1);
    }).detach();
}

void SessionHub::acceptNew() {
    for (;;) {
        net::Socket sock = net::acceptClient(listener_);
        if (!sock.valid()) {
            break;
        }
        auto client = std::make_unique<ClientSession>(std::move(sock));
        client->id = nextClientId_++;
        client->label = client->conn.peer();
        client->lastHeartbeatMs = nowMs();
        std::fprintf(stdout, "[photon-backend] client connected %s\n", client->label.c_str());
        clients_.push_back(std::move(client));
    }
}

void SessionHub::sendControl(ClientSession& client, const ctrl::Message& msg) {
    const auto before = client.conn.outboundBytes();
    client.conn.queueText(ctrl::serialize(msg));
    noteQueued(client, msg.type == ctrl::Type::HistoryResult, before);
}

void SessionHub::sendBinary(ClientSession& client, proto::MsgType type, std::span<const std::uint8_t> payload) {
    const auto frame = proto::encodeFrame(type, binarySeq_++, payload);
    const auto before = client.conn.outboundBytes();
    client.conn.queueBinary(frame);
    noteQueued(client, type == proto::MsgType::HistoryChunk, before);
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
            // One extra RPC on the retained locate socket. Eight range reads
            // plus live reconnect used to trip ClientGate and leave the UI on
            // "loading GOP pointers". Prefer the fattest pointer (720p).
            const vidx::GopRow* best = nullptr;
            for (const auto& row : reply.rows) {
                if (vidx::rowIsContinuation(row) || row.byteLength == 0) {
                    continue;
                }
                if (msg->startMs == 0) {
                    // Live t=0: the locate Entry is first. Prefetch rows can
                    // be RTCP-future GOPs that sort later; picking max
                    // startMs froze Live on a 49-minute-ahead still (0 fps).
                    if (best == nullptr) {
                        best = &row;
                    }
                    continue;
                }
                if (best == nullptr || row.byteLength > best->byteLength) {
                    best = &row;
                }
            }
            vidx::GopRing loaded;
            bool appended = false;
            const bool livePoll = msg->startMs == 0;
            const bool sameGop = best != nullptr && !client.videoCache.nals().empty() &&
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
                    // Stamp the tail over its share of the row's window. Row
                    // offsets are NAL boundaries, so this resumes on a start
                    // code; a short read cannot, which is why it falls through
                    // to the full re-read below.
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
            if (!appended && best != nullptr) {
                std::vector<std::uint8_t> gop;
                std::string rangeError;
                if (videoRelay_.readRange(best->segmentId, best->byteOffset, best->byteLength, gop, &rangeError)) {
                    video::forEachStampedGopNal(gop, best->startMs, best->endMs,
                                                [&](std::uint64_t ms, std::uint8_t flags,
                                                    std::span<const std::uint8_t> nal) {
                                                    if (video::nalIsTinyPslice(nal)) {
                                                        return;
                                                    }
                                                    loaded.push(ms, flags, nal);
                                                });
                    if (!loaded.nals().empty()) {
                        client.videoGopSegment = best->segmentId;
                        client.videoGopOffset = best->byteOffset;
                        client.videoGopSent = static_cast<std::uint32_t>(gop.size());
                        client.videoGopLive = true;
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
        case ctrl::Type::SignalCatalog:
        case ctrl::Type::VideoStatus:
        case ctrl::Type::HistoryResult:
        case ctrl::Type::AnalysisComputeResult:
        case ctrl::Type::CatalogResult:
            sendControl(client, ctrl::errorAlert("bad_message", "server-bound message type not accepted"));
            break;
    }
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

void SessionHub::serviceClient(ClientSession& client, std::uint64_t nowTick, short revents) {
    if ((revents & (net::kPollHup | net::kPollErr)) != 0 && (revents & net::kPollIn) == 0) {
        client.conn.queueClose(1001, "peer error");
        return;
    }

    if (!client.conn.handshakeComplete()) {
        const auto hs = client.conn.readHandshake();
        if (hs == ws::HandshakeStatus::Incomplete) {
            return;
        }
        if (hs == ws::HandshakeStatus::HttpExport) {
            std::string target = client.conn.httpTarget();
            net::Socket sock = client.conn.releaseSocket();
            client.failed = true;
            startCsvExport(std::move(sock), std::move(target));
            return;
        }
        if (hs == ws::HandshakeStatus::BadRequest) {
            const char* resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            client.conn.socket().sendSome(resp, std::strlen(resp));
            client.failed = true;
            return;
        }
        if (!client.conn.writeHandshakeResponse()) {
            return;
        }
        std::fprintf(stdout, "[photon-backend] websocket handshake ok %s\n", client.label.c_str());
    }

    if ((revents & net::kPollIn) != 0 || client.conn.handshakeComplete()) {
        if (!client.conn.readAvailable()) {
            client.conn.queueClose(1001, "read error");
            return;
        }
        while (auto frame = client.conn.takeFrame()) {
            if (frame->opcode == ws::Opcode::Text) {
                const std::string_view text(reinterpret_cast<const char*>(frame->payload.data()), frame->payload.size());
                handleText(client, text, nowTick);
            } else if (frame->opcode == ws::Opcode::Close) {
                std::fprintf(stdout, "[photon-backend] client close %s\n", client.label.c_str());
            }
        }
    }

    if ((revents & net::kPollOut) != 0 || client.conn.hasOutbound()) {
        if (!client.conn.flushOutbound()) {
            client.conn.queueClose(1001, "write error");
        }
    }
}

void SessionHub::dropClosed() {
    const auto before = clients_.size();
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [this](const std::unique_ptr<ClientSession>& client) {
                                      if (!client->failed && !client->conn.wantsClose() && client->conn.socket().valid()) {
                                          return false;
                                      }
                                      if (client->txArmed) {
                                          disarm(*client, "client_disconnect");
                                      }
                                      if (client->videoSubscribed) {
                                          videoRelay_.unsubscribe(client->id);
                                      }
                                      for (auto& [id, slot] : client->historyQueries) {
                                          (void)id;
                                          slot.cancelled = true;
                                          if (slot.cancel) {
                                              slot.cancel->store(true);
                                          }
                                      }
                                      if (historyRemote_.enabled()) {
                                          historyRemote_.cancelHistory(client->id, 0);
                                      }
                                      std::fprintf(stdout, "[photon-backend] client dropped %s\n", client->label.c_str());
                                      return true;
                                  }),
                   clients_.end());
    if (clients_.size() != before) {
        refreshDecodeWanted();
    }
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

void SessionHub::streamVideo() {
    auto frames = videoRelay_.takeFrames();
    if (frames.empty()) {
        return;
    }
    static std::uint64_t wsVideo = 0;
    wsVideo += frames.size();
    if (wsVideo <= 3 || wsVideo % 200 == 0) {
        std::fprintf(stdout, "[photon-backend] video ws nals=%llu pending=%zu clients=%zu\n",
                     static_cast<unsigned long long>(wsVideo), frames.size(), clients_.size());
        std::fflush(stdout);
    }
    for (const auto& frame : frames) {
        proto::VideoFrame vf;
        vf.timestampMs = frame.timestampMs;
        vf.flags = frame.flags;
        vf.nal = frame.payload;
        const auto payload = proto::encodeVideoFrame(vf);
        const bool key = proto::videoKeyframe(frame.flags);
        const bool cfg = proto::videoConfig(frame.flags);
        for (auto& client : clients_) {
            if (!client->helloOk || !client->videoSubscribed) {
                continue;
            }
            // Slow-WS-client: drop non-keyframe (and non-config) when the send buffer is congested.
            // Config NALs stay — they are tiny and required for decoder init.
            if (client->conn.outboundBytes() > kVideoCongestBytes && !key && !cfg) {
                videoRelay_.noteDropped();
                continue;
            }
            sendBinary(*client, proto::MsgType::VideoFrame, payload);
        }
    }
}

void SessionHub::streamVideoStatus(std::uint64_t nowTick, bool force) {
    const bool changed = videoRelay_.consumeStatusChanged();
    if (!force && !changed) {
        if (!videoRelay_.hasSubscribers()) {
            return;
        }
        if (lastVideoStatusMs_ != 0 && nowTick - lastVideoStatusMs_ < 1000) {
            return;
        }
    }
    lastVideoStatusMs_ = nowTick;
    const auto msg = videoRelay_.statusMessage();
    for (auto& client : clients_) {
        if (client->helloOk && client->videoSubscribed) {
            sendControl(*client, msg);
        }
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

void SessionHub::tick(std::uint64_t nowTick) {
    std::vector<net::PollFd> fds;
    fds.push_back(net::PollFd{listener_.native(), net::kPollIn, 0});
    for (auto& client : clients_) {
        short events = net::kPollIn;
        if (client->conn.hasOutbound()) {
            events = static_cast<short>(events | net::kPollOut);
        }
        fds.push_back(net::PollFd{client->conn.socket().native(), events, 0});
    }
    sources_.attachPoll(fds);
    videoRelay_.attachPoll(fds);
    net::poll(fds.data(), fds.size(), 5);

    const std::size_t nClients = clients_.size();
    if (!fds.empty() && (fds[0].revents & net::kPollIn) != 0) {
        acceptNew();
    }

    for (std::size_t i = 0; i < clients_.size(); ++i) {
        short revents = 0;
        if (i < nClients) {
            const std::size_t fdIndex = i + 1;
            if (fdIndex < fds.size() && fds[fdIndex].fd == clients_[i]->conn.socket().native()) {
                revents = fds[fdIndex].revents;
            }
        }
        serviceClient(*clients_[i], nowTick, revents);
        flushHistory(*clients_[i], nowTick);
        if (clients_[i]->conn.hasOutbound()) {
            const auto before = clients_[i]->conn.outboundBytes();
            clients_[i]->conn.flushOutbound();
            noteDrained(*clients_[i], before);
        }
    }

    sources_.onPoll(fds);
    videoRelay_.onPoll(fds);
    sources_.tick(nowTick);
    tickClientReplays(nowTick);
    if (sources_.hasLog()) {
        for (auto& client : clients_) {
            if (client->helloOk && client->logPointer && sources_.consumeReplaySnapshotDue(client->replay)) {
                pushSnapshot(*client);
            }
        }
    }
    if (sources_.consumeSnapshotDue()) {
        for (auto& client : clients_) {
            if (client->helloOk && !client->logPointer) {
                pushSnapshot(*client);
            }
        }
    }
    videoRelay_.tick(nowTick);
    streamTelemetry(nowTick);
    streamCan(nowTick);
    streamTimeline(nowTick);
    streamVideo();
    streamVideoStatus(nowTick, false);
    checkTxWatchdog(nowTick);
    pollTrackSimJobs();
    pollAnalysisJobs();
    pollHistoryJobs();
    historyRemote_.tick(nowTick);
    flushHistoryRemote();
    tickRetention(nowTick);
    dropClosed();

    for (auto& client : clients_) {
        if (client->conn.hasOutbound()) {
            const auto before = client->conn.outboundBytes();
            client->conn.flushOutbound();
            noteDrained(*client, before);
        }
    }
}

ClientSession* SessionHub::findClient(std::uint64_t id) {
    for (auto& client : clients_) {
        if (client && client->id == id) {
            return client.get();
        }
    }
    return nullptr;
}

void SessionHub::enqueueTrackSim(ClientSession& client, const ctrl::Message& req) {
    if (!client.helloOk) {
        sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
        return;
    }
    if (!(req.timeBudgetS > 0.0)) {
        sendControl(client, ctrl::errorAlert("bad_message", "time_budget_s must be positive"));
        return;
    }

    tracksim::SolveInput input;
    input.timeBudgetS = req.timeBudgetS;
    input.initialSoc = req.initialSoc;
    input.car = tracksim::applyCarOverride(tracksim::defaultCar(), req.carOverride);

    if (req.track) {
        input.track = *req.track;
    } else if (!req.trackId.empty()) {
        std::string loadError;
        auto track = catalog_.load(req.trackId, &loadError);
        if (!track) {
            sendControl(client, ctrl::errorAlert("unknown_track", loadError.empty() ? "unknown track_id" : loadError));
            return;
        }
        input.track = std::move(*track);
    } else {
        sendControl(client, ctrl::errorAlert("bad_message", "track_id or inline track required"));
        return;
    }

    const std::string trackId = input.track.id.empty() ? req.trackId : input.track.id;
    const std::uint64_t clientId = client.id;
    trackSimJobs_.push_back(TrackSimJob{
        clientId,
        std::async(std::launch::async,
                   [input, trackId]() { return ctrl::trackSimResult(tracksim::solve(input), trackId); }),
    });
}

void SessionHub::pollTrackSimJobs() {
    for (auto it = trackSimJobs_.begin(); it != trackSimJobs_.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto msg = it->future.get();
        if (auto* client = findClient(it->clientId)) {
            sendControl(*client, msg);
        }
        it = trackSimJobs_.erase(it);
    }
}

void SessionHub::handleAnalysisCompute(ClientSession& client, const ctrl::Message& req) {
    if (!client.helloOk) {
        sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
        return;
    }
    if (req.jobId.empty()) {
        sendControl(client, ctrl::errorAlert("bad_message", "job_id required"));
        return;
    }

    analysis::Addr addr;
    if (!analysis::parseAddr(analysisComputeAddr_, addr)) {
        sendControl(client, analysis::offlineResult(req.jobId));
        return;
    }

    int inflightClient = 0;
    for (const auto& job : analysisJobs_) {
        if (job.clientId == client.id) {
            ++inflightClient;
        }
    }
    if (inflightClient >= analysis::kMaxInflightPerClient) {
        sendControl(client, analysis::busyResult(req.jobId));
        return;
    }
    if (static_cast<int>(analysisJobs_.size()) >= analysis::kMaxInflightGlobal) {
        sendControl(client, analysis::busyResult(req.jobId));
        return;
    }

    analysis::JobSpec spec;
    spec.jobId = req.jobId;
    spec.clientId = sidecarClientIdFor(client);
    spec.sessionId = req.sessionId;
    spec.dbcHash = req.dbcHash.empty() ? req.dbcId : req.dbcHash;
    spec.signalIds = req.signalIds;
    spec.t0Ms = req.t0Ms != 0 ? req.t0Ms : req.fromMs;
    spec.t1Ms = req.t1Ms != 0 ? req.t1Ms : req.toMs;
    spec.maxPoints = req.maxPoints;
    spec.prompt = req.prompt;
    spec.recipeId = req.recipeId;
    spec.kind = req.computeKind.empty() ? "grok" : req.computeKind;
    spec.timeoutS = analysis::kDefaultTimeoutS;

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    const std::uint64_t clientId = client.id;
    analysisJobs_.push_back(AnalysisJob{
        clientId,
        req.jobId,
        spec.clientId,
        cancel,
        std::async(std::launch::async, [addr, spec, cancel]() { return analysis::runJob(addr, spec, cancel.get()); }),
    });
}

void SessionHub::handleAnalysisCancel(ClientSession& client, const ctrl::Message& req) {
    if (!client.helloOk) {
        sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
        return;
    }
    if (req.jobId.empty()) {
        sendControl(client, ctrl::errorAlert("bad_message", "job_id required"));
        return;
    }
    bool found = false;
    for (auto& job : analysisJobs_) {
        if (job.clientId == client.id && job.jobId == req.jobId) {
            found = true;
            if (job.cancel) {
                job.cancel->store(true);
            }
        }
    }
    if (!found) {
        sendControl(client, ctrl::analysisComputeResult(req.jobId, "failed", {}, "unknown_job", "job not found"));
        return;
    }
}

void SessionHub::pollAnalysisJobs() {
    for (auto it = analysisJobs_.begin(); it != analysisJobs_.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto msg = it->future.get();
        if (auto* client = findClient(it->clientId)) {
            sendControl(*client, msg);
        }
        it = analysisJobs_.erase(it);
    }
}

void SessionHub::cancelAllAnalysisJobs() {
    analysis::Addr addr;
    const bool haveAddr = analysis::parseAddr(analysisComputeAddr_, addr);
    for (auto& job : analysisJobs_) {
        if (job.cancel) {
            job.cancel->store(true);
        }
        if (haveAddr && !job.jobId.empty() && !job.sidecarClientId.empty()) {
            analysis::cancelJob(addr, job.jobId, job.sidecarClientId);
        }
        analysis::removeBundle((std::filesystem::temp_directory_path() / "photon-analysis-compute" / job.jobId /
                                "bundle.json")
                                   .string());
    }
    for (auto& job : analysisJobs_) {
        if (job.future.valid()) {
            job.future.wait();
        }
    }
    analysisJobs_.clear();
}

namespace {

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

void SessionHub::noteQueued(ClientSession& client, bool history, std::size_t beforeBytes) {
    const auto now = client.conn.outboundBytes();
    if (now > beforeBytes) {
        client.outboundTags.push_back(OutboundTag{history, now - beforeBytes});
    }
}

void SessionHub::noteDrained(ClientSession& client, std::size_t beforeBytes) {
    const auto now = client.conn.outboundBytes();
    if (beforeBytes <= now) {
        return;
    }
    std::size_t drained = beforeBytes - now;
    while (drained > 0 && !client.outboundTags.empty()) {
        auto& tag = client.outboundTags.front();
        if (tag.bytes <= drained) {
            drained -= tag.bytes;
            client.outboundTags.pop_front();
        } else {
            tag.bytes -= drained;
            drained = 0;
        }
    }
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

bool SessionHub::liveSource() const {
    if (!writeLiveTail_) {
        return false;
    }
    const auto kind = sources_.kind();
    return kind == ctrl::SourceKind::DaqTcp || kind == ctrl::SourceKind::Pcan;
}

void SessionHub::recoverCrashedLive() {
    if (tsdbDir_.empty()) {
        return;
    }
    std::string error;
    const auto n = tsdb::recoverCrashedSessions(tsdbDir_, &error);
    if (n > 0) {
        std::fprintf(stdout, "[photon-backend] recovered %u crashed live session(s)\n", n);
    }
    if (!error.empty()) {
        std::fprintf(stderr, "[photon-backend] live recover: %s\n", error.c_str());
    }
}

void SessionHub::startLiveWriter(std::uint64_t firstMs) {
    if (!liveSource() || !dbcLoaded_ || tsdbDir_.empty() || liveWriter_) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(tsdbDir_, ec);
    int seq = 0;
    std::string id;
    for (;;) {
        id = tsdb::sessionIdFromMs(firstMs, seq);
        if (!std::filesystem::exists(std::filesystem::path(tsdbDir_) / id, ec)) {
            break;
        }
        ++seq;
        if (seq > 99) {
            return;
        }
    }
    auto writer = std::make_unique<tsdb::LiveTailWriter>();
    tsdb::LiveOptions opts;
    std::string error;
    if (!writer->begin(tsdbDir_, id, dbc_, opts, &error)) {
        std::fprintf(stderr, "[photon-backend] live writer begin failed: %s\n", error.c_str());
        return;
    }
    liveWriter_ = std::move(writer);
    liveSessionId_ = std::move(id);
    liveLastMs_ = firstMs;
    std::fprintf(stdout, "[photon-backend] live tail session %s\n", liveSessionId_.c_str());
}

void SessionHub::stopLiveWriter() {
    if (!liveWriter_) {
        return;
    }
    std::string error;
    const auto dir = liveWriter_->dir();
    if (!liveWriter_->end(&error)) {
        std::fprintf(stderr, "[photon-backend] live writer end: %s\n", error.empty() ? "failed" : error.c_str());
    }
    publishLiveCatalog();
    tsdb::CompactionOptions copts;
    tsdb::CompactionReport crep;
    std::string cerr;
    if (!dir.empty() && !tsdb::compactSession(dir, copts, crep, &cerr) && !cerr.empty()) {
        std::fprintf(stderr, "[photon-backend] live compact: %s\n", cerr.c_str());
    }
    if (!liveSessionId_.empty()) {
        historyStores_.erase(liveSessionId_);
    }
    liveWriter_.reset();
    liveSessionId_.clear();
    liveLastMs_ = 0;
}

void SessionHub::publishLiveCatalog() {
    if (!liveWriter_ || liveSessionId_.empty()) {
        return;
    }
    const auto& man = liveWriter_->store().manifest();
    tsdb::upsertCatalog(tsdbDir_, liveSessionId_, man.minMs, man.maxMs, man.dbcSha, nullptr);
}

void SessionHub::feedLive(std::span<const proto::CanMonitorItem> frames) {
    if (!liveSource() || !dbcLoaded_ || tsdbDir_.empty()) {
        return;
    }
    for (const auto& frame : frames) {
        if (liveWriter_ && tsdb::isSessionBoundary(frame.timestampMs, liveLastMs_)) {
            stopLiveWriter();
        }
        if (!liveWriter_) {
            startLiveWriter(frame.timestampMs);
        }
        if (!liveWriter_) {
            return;
        }
        liveWriter_->onFrame(frame.canId, frame.dlc, frame.data, frame.timestampMs);
        if (liveWriter_->failed()) {
            std::fprintf(stderr, "[photon-backend] live writer: %s\n", liveWriter_->lastError().c_str());
            liveWriter_.reset();
            liveSessionId_.clear();
            return;
        }
        if (frame.timestampMs > liveLastMs_) {
            liveLastMs_ = frame.timestampMs;
        }
    }
    if (liveWriter_ && liveLastMs_ >= lastLiveCatalogMs_ + 5000) {
        lastLiveCatalogMs_ = liveLastMs_;
        publishLiveCatalog();
    }
}

void SessionHub::tickRetention(std::uint64_t nowMs) {
    if (tsdbDir_.empty() || nowMs < lastRetentionMs_ + 600000) {
        return;
    }
    lastRetentionMs_ = nowMs;
    tsdb::RetentionPolicy policy;
    if (const char* env =
#if defined(_MSC_VER)
            []() -> const char* {
                static char buf[32];
                size_t n = 0;
                return getenv_s(&n, buf, sizeof(buf), "PHOTON_L0_MAX_AGE_DAYS") == 0 && n > 1 ? buf : nullptr;
            }()
#else
            std::getenv("PHOTON_L0_MAX_AGE_DAYS")
#endif
    ) {
        const auto days = std::strtoull(env, nullptr, 10);
        if (days > 0) {
            policy.l0MaxAgeMs = days * 86400000ull;
        }
    }
    if (policy.l0MaxAgeMs == 0 && policy.l0BudgetBytes == 0) {
        return;
    }
    std::string error;
    const auto report = tsdb::applyRetention(tsdbDir_, policy, nowMs, &error);
    if (!report.ok && !error.empty()) {
        std::fprintf(stderr, "[photon-backend] retention: %s\n", error.c_str());
    } else if (report.l0BytesFreed > 0) {
        std::fprintf(stdout, "[photon-backend] retention freed %llu L0 bytes\n",
                     static_cast<unsigned long long>(report.l0BytesFreed));
    }
}

}  // namespace photon
