#pragma once

#include "photon/control.hpp"
#include "photon_ui/arena.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace photon::ui {

class Client {
public:
    enum class State { Disconnected, Connecting, Connected, HelloOk };

    explicit Client(ClientArena& arena);

    void tick(double nowMs);
    void connect();
    void disconnect(const char* why);
    void setUrl(std::string url);

    bool send(const ctrl::Message& msg);
    void selectSource(ctrl::SourceKind kind, std::string host, std::uint16_t port, std::string path);
    void setSubscribed(std::uint32_t id, bool on);
    void ensureSubscribed(std::span<const std::uint32_t> ids);
    void sendTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs);
    void requestTrackSimList();
    void requestTrackSim(std::string trackId, double timeBudgetS, double initialSoc,
                         tracksim::CarOverride car = {});
    void subscribeVideo(ctrl::VideoMode mode, std::uint64_t startMs = 0);
    void stopVideo();

    State state() const { return state_; }
    const char* stateLabel() const;
    bool helloOk() const { return state_ == State::HelloOk; }
    const std::unordered_set<std::uint32_t>& subscribed() const { return subscribed_; }
    const std::string& url() const { return url_; }

    void handleOpen();
    void handleClose(const char* why, bool error);
    void handleText(std::string_view text);
    void handleBinary(std::span<const std::uint8_t> bytes);

private:
    ClientArena& arena_;
    std::string url_;
    int socket_ = 0;
    State state_ = State::Disconnected;
    int backoffIndex_ = 0;
    double nextReconnectMs_ = 0.0;
    double lastHeartbeatMs_ = 0.0;
    std::unordered_set<std::uint32_t> subscribed_;
    ctrl::SourceKind pendingSource_ = ctrl::SourceKind::Synthetic;
    std::string pendingHost_ = "127.0.0.1";
    std::uint16_t pendingPort_ = 6500;
    std::string pendingPath_;
    bool videoWanted_ = false;
    ctrl::VideoMode videoMode_ = ctrl::VideoMode::Live;
    std::uint64_t videoStartMs_ = 0;

    void attachCallbacks();
    void scheduleReconnect(double nowMs);
    void resubscribe();
    void sendSubscriptions();
    void adoptActiveSource(const ctrl::Message::ActiveSource& active);
    void applyHelloAck(const ctrl::Message& msg);
};

}  // namespace photon::ui
