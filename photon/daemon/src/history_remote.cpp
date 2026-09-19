#include "photon/history_remote.hpp"

#include "photon/base64.hpp"
#include "photon/protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace photon {
namespace {

bool loopbackName(std::string_view host) {
    return host.empty() || host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

}  // namespace

bool hostIsLoopback(std::string_view host) {
    return loopbackName(host);
}

bool parseHostPort(std::string_view spec, std::string& host, std::uint16_t& port) {
    while (!spec.empty() && (spec.front() == ' ' || spec.front() == '\t')) {
        spec.remove_prefix(1);
    }
    while (!spec.empty() && (spec.back() == ' ' || spec.back() == '\t')) {
        spec.remove_suffix(1);
    }
    if (spec.empty() || spec == "off" || spec == "-" || spec == "0") {
        host.clear();
        port = 0;
        return true;
    }
    const auto colon = spec.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= spec.size()) {
        return false;
    }
    host.assign(spec.substr(0, colon));
    char* end = nullptr;
    const unsigned long v = std::strtoul(std::string(spec.substr(colon + 1)).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v < 1 || v > 65535ul) {
        return false;
    }
    port = static_cast<std::uint16_t>(v);
    return !host.empty();
}

void HistoryRemote::setTarget(std::string host, std::uint16_t port, std::string path, std::string password) {
    close();
    host_ = std::move(host);
    port_ = port == 0 ? 8080 : port;
    path_ = path.empty() ? "/ws" : std::move(path);
    password_ = std::move(password);
    catalogReady_ = false;
    catalog_ = ctrl::catalogResult({});
    nextRetryMs_ = 0;
    backoffMs_ = 500;
    queryClient_.clear();
    videoClients_.clear();
    videoLocateClient_ = 0;
    pending_.clear();
    out_.clear();
}

ctrl::Message HistoryRemote::catalogMessage() const {
    return catalog_;
}

void HistoryRemote::requestCatalog() {
    if (state_ == State::Ready && conn_) {
        sendText(ctrl::catalogList());
    }
}

void HistoryRemote::requestHistory(std::uint64_t clientId, const ctrl::Message& query) {
    queryClient_[query.queryId] = clientId;
    if (query.queryId <= 3 || (query.queryId % 32u) == 0) {
        std::fprintf(stdout, "[photon-backend] history-remote query id=%u session=%s from=%llu to=%llu sigs=%zu\n",
                     query.queryId, query.sessionId.c_str(), static_cast<unsigned long long>(query.fromMs),
                     static_cast<unsigned long long>(query.toMs), query.signalIds.size());
        std::fflush(stdout);
    }
    if (state_ == State::Ready && conn_) {
        sendText(query);
        return;
    }
    pending_.push_back({clientId, query});
}

void HistoryRemote::requestVideoLocate(std::uint64_t clientId, const ctrl::Message& locate) {
    videoClients_.insert(clientId);
    videoLocateClient_ = clientId;
    // Do not video_subscribe on the remote hop. MACBOOK live :6700 flaps
    // (connect/eof) and streams video_status every tick.
    std::fprintf(stdout, "[photon-backend] history-remote video_locate at=%llu\n",
                 static_cast<unsigned long long>(locate.startMs));
    std::fflush(stdout);
    if (state_ == State::Ready && conn_) {
        sendText(locate);
        return;
    }
    pending_.push_back({clientId, locate});
}

void HistoryRemote::sendCredit(std::uint32_t queryId, std::uint64_t bytes) {
    if (bytes == 0 || state_ != State::Ready || !conn_) {
        return;
    }
    sendText(ctrl::historyCredit(queryId, bytes));
}

void HistoryRemote::cancelHistory(std::uint64_t clientId, std::uint32_t queryId) {
    if (queryId == 0) {
        for (auto it = queryClient_.begin(); it != queryClient_.end();) {
            if (it->second == clientId) {
                if (state_ == State::Ready && conn_) {
                    sendText(ctrl::historyCancel(it->first));
                }
                it = queryClient_.erase(it);
            } else {
                ++it;
            }
        }
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [clientId](const auto& item) { return item.first == clientId; }),
                       pending_.end());
        return;
    }
    const auto it = queryClient_.find(queryId);
    if (it != queryClient_.end() && it->second == clientId) {
        queryClient_.erase(it);
    }
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [clientId, queryId](const auto& item) {
                                      return item.first == clientId && item.second.queryId == queryId;
                                  }),
                   pending_.end());
    if (state_ == State::Ready && conn_) {
        sendText(ctrl::historyCancel(queryId));
    }
}

std::vector<HistoryRemote::Delivery> HistoryRemote::takeDeliveries() {
    std::vector<Delivery> out;
    out.swap(out_);
    return out;
}

void HistoryRemote::close() {
    conn_.reset();
    state_ = State::Idle;
    catalogReady_ = false;
}

void HistoryRemote::sendText(const ctrl::Message& msg) {
    if (!conn_) {
        return;
    }
    conn_->queueText(ctrl::serialize(msg));
    conn_->flushOutbound();
}

std::uint64_t HistoryRemote::clientForQuery(std::uint32_t queryId) const {
    const auto it = queryClient_.find(queryId);
    return it == queryClient_.end() ? 0 : it->second;
}

void HistoryRemote::connect(std::uint64_t nowMs) {
    if (!enabled() || nowMs < nextRetryMs_) {
        return;
    }
    auto sock = net::connectTcpTimed(host_, port_, 800);
    if (!sock.valid()) {
        nextRetryMs_ = nowMs + backoffMs_;
        backoffMs_ = std::min<std::uint64_t>(backoffMs_ * 2, 8000);
        std::fprintf(stdout, "[photon-backend] history-remote connect %s:%u failed\n", host_.c_str(),
                     static_cast<unsigned>(port_));
        std::fflush(stdout);
        return;
    }
    sock.setNonBlocking(true);
    sock.setNoDelay(true);
    conn_ = std::make_unique<ws::Connection>(std::move(sock));
    conn_->setClientMode(true);
    std::string basic;
    if (!password_.empty()) {
        const std::string raw = "photon:" + password_;
        basic = crypto::base64Encode(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()));
    }
    const std::string hostHdr = host_ + ":" + std::to_string(port_);
    if (!conn_->beginClientHandshake(path_, hostHdr, basic)) {
        close();
        nextRetryMs_ = nowMs + backoffMs_;
        return;
    }
    state_ = State::Handshake;
    std::fprintf(stdout, "[photon-backend] history-remote handshake %s:%u%s\n", host_.c_str(),
                 static_cast<unsigned>(port_), path_.c_str());
    std::fflush(stdout);
}

void HistoryRemote::handleRemoteText(std::string_view text) {
    std::string err;
    const auto msg = ctrl::parse(text, &err);
    if (!msg) {
        return;
    }
    if (msg->type == ctrl::Type::HelloAck) {
        state_ = State::Ready;
        backoffMs_ = 500;
        sendText(ctrl::catalogList());
        for (auto& item : pending_) {
            sendText(item.second);
        }
        pending_.clear();
        std::fprintf(stdout, "[photon-backend] history-remote hello_ack catalog_list\n");
        std::fflush(stdout);
        return;
    }
    if (msg->type == ctrl::Type::CatalogResult) {
        catalog_ = *msg;
        catalogReady_ = true;
        std::fprintf(stdout, "[photon-backend] history-remote catalog n=%zu\n", msg->catalogSessions.size());
        std::fflush(stdout);
        Delivery d;
        d.text = *msg;
        out_.push_back(std::move(d));
        return;
    }
    if (msg->type == ctrl::Type::HistoryResult) {
        std::fprintf(stdout, "[photon-backend] history-remote result id=%u truncated=%d series=%zu\n",
                     msg->queryId, msg->truncated ? 1 : 0, msg->historySeries.size());
        std::fflush(stdout);
        Delivery d;
        d.clientId = clientForQuery(msg->queryId);
        d.text = *msg;
        out_.push_back(std::move(d));
        return;
    }
    if (msg->type == ctrl::Type::ErrorAlert) {
        std::fprintf(stdout, "[photon-backend] history-remote rx %s %s %s\n",
                     std::string(ctrl::typeName(msg->type)).c_str(), msg->code.c_str(), msg->message.c_str());
        std::fflush(stdout);
        Delivery d;
        d.clientId = videoLocateClient_;
        d.text = *msg;
        out_.push_back(std::move(d));
        return;
    }
}

void HistoryRemote::handleRemoteBinary(std::vector<std::uint8_t> bytes) {
    const auto frame = proto::decodeFrame(bytes);
    if (!frame) {
        return;
    }
    const auto type = static_cast<proto::MsgType>(frame->header.msgType);
    if (type == proto::MsgType::VideoFrame || type == proto::MsgType::VideoLocateReply) {
        std::fprintf(stdout, "[photon-backend] history-remote rx binary type=0x%02x bytes=%zu\n",
                     static_cast<unsigned>(frame->header.msgType), bytes.size());
        std::fflush(stdout);
    }
    Delivery d;
    if (type == proto::MsgType::HistoryChunk) {
        const auto chunk = proto::decodeHistoryChunk(frame->payload);
        d.clientId = chunk ? clientForQuery(chunk->queryId) : 0;
    } else if (type == proto::MsgType::VideoFrame || type == proto::MsgType::VideoLocateReply) {
        d.clientId = videoLocateClient_;
    } else {
        return;
    }
    d.binary = std::move(bytes);
    out_.push_back(std::move(d));
}

void HistoryRemote::tick(std::uint64_t nowMs) {
    if (!enabled()) {
        return;
    }
    if (!conn_) {
        connect(nowMs);
        return;
    }
    if (!conn_->readAvailable()) {
        close();
        nextRetryMs_ = nowMs + backoffMs_;
        return;
    }
    if (state_ == State::Handshake) {
        const auto hs = conn_->readClientHandshake();
        if (hs == ws::HandshakeStatus::Incomplete) {
            return;
        }
        if (hs != ws::HandshakeStatus::Ok) {
            std::fprintf(stdout, "[photon-backend] history-remote handshake rejected\n");
            std::fflush(stdout);
            close();
            nextRetryMs_ = nowMs + backoffMs_;
            return;
        }
        state_ = State::Hello;
        sendText(ctrl::hello("laptop-history"));
        lastHelloMs_ = nowMs;
        return;
    }
    if (state_ == State::Hello && nowMs - lastHelloMs_ > 8000) {
        close();
        nextRetryMs_ = nowMs + backoffMs_;
        return;
    }
    for (;;) {
        auto frame = conn_->takeFrame();
        if (!frame) {
            break;
        }
        if (frame->opcode == ws::Opcode::Text) {
            handleRemoteText(std::string_view(reinterpret_cast<const char*>(frame->payload.data()),
                                              frame->payload.size()));
        } else if (frame->opcode == ws::Opcode::Binary) {
            handleRemoteBinary(std::move(frame->payload));
        } else if (frame->opcode == ws::Opcode::Close) {
            close();
            nextRetryMs_ = nowMs + backoffMs_;
            return;
        }
    }
    if (conn_->hasOutbound()) {
        conn_->flushOutbound();
    }
    if (conn_->wantsClose()) {
        close();
        nextRetryMs_ = nowMs + backoffMs_;
    }
}

}  // namespace photon
