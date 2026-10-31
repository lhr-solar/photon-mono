#pragma once
#include <string>
#include "wire/control.hpp"
namespace photon::ui {
struct UiContext;
class ConnectionSettings {
public:
    void draw(UiContext& ui);

private:
    int sourceIndex_ = 1;
    char host_[64] = "127.0.0.1";
    int port_ = 6500;
    char path_[512] = "";
    char url_[128] = "ws://127.0.0.1:8080/ws";
    int pcanChannel_ = 1;
    int pcanBaudIndex_ = 1;
    bool pcanListenOnly_ = true;
    bool hydrated_ = false;
    ctrl::SourceKind syncedSource_ = ctrl::SourceKind::None;
    std::string syncedHost_;
    std::uint16_t syncedPort_ = 0;
    std::string syncedPath_;
    int dbcIndex_ = 0;
    char dbcPath_[512] = "";
    std::string syncedDbcId_;
    std::string syncedDbcPath_;
};
}  // namespace photon::ui
