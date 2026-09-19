#pragma once

#include "photon/control.hpp"
#include "photon/net.hpp"
#include "photon/websocket.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace photon {

// Pulls catalog.json / history tiles from the ingest photon-backend (MACBOOK)
// over its public /ws hop. Laptop daq_tcp keeps live CAN local; TSDB stays remote.
class HistoryRemote {
public:
    struct Delivery {
        std::uint64_t clientId = 0;
        std::optional<ctrl::Message> text;
        std::vector<std::uint8_t> binary;
    };

    void setTarget(std::string host, std::uint16_t port, std::string path, std::string password);
    bool enabled() const { return !host_.empty(); }
    bool catalogReady() const { return catalogReady_; }
    ctrl::Message catalogMessage() const;
    void requestCatalog();
    void requestHistory(std::uint64_t clientId, const ctrl::Message& query);
    void requestVideoLocate(std::uint64_t clientId, const ctrl::Message& locate);
    void sendCredit(std::uint32_t queryId, std::uint64_t bytes);
    void cancelHistory(std::uint64_t clientId, std::uint32_t queryId);
    void tick(std::uint64_t nowMs);
    std::vector<Delivery> takeDeliveries();

private:
    enum class State : std::uint8_t { Idle, Connecting, Handshake, Hello, Ready };

    void close();
    void connect(std::uint64_t nowMs);
    void sendText(const ctrl::Message& msg);
    void handleRemoteText(std::string_view text);
    void handleRemoteBinary(std::vector<std::uint8_t> bytes);
    std::uint64_t clientForQuery(std::uint32_t queryId) const;

    std::string host_;
    std::uint16_t port_ = 8080;
    std::string path_ = "/ws";
    std::string password_;
    State state_ = State::Idle;
    std::unique_ptr<ws::Connection> conn_;
    bool catalogReady_ = false;
    ctrl::Message catalog_;
    std::uint64_t nextRetryMs_ = 0;
    std::uint64_t backoffMs_ = 500;
    std::uint64_t lastHelloMs_ = 0;
    std::unordered_map<std::uint32_t, std::uint64_t> queryClient_;
    std::unordered_set<std::uint64_t> videoClients_;
    std::uint64_t videoLocateClient_ = 0;
    std::vector<std::pair<std::uint64_t, ctrl::Message>> pending_;
    std::vector<Delivery> out_;

};

bool parseHostPort(std::string_view spec, std::string& host, std::uint16_t& port);
bool hostIsLoopback(std::string_view host);

}  // namespace photon
