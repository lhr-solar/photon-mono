#include "hw/pcan.hpp"
#include "net/net.hpp"
#include "net/base64.hpp"
#include "base/json.hpp"
#include "wire/canp.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace {
using namespace photon;
std::string request(std::uint16_t port, const std::string& password, const char* method,
                    const char* path, const std::string& body) {
    auto socket = net::connectTcpTimed("127.0.0.1", port, 1000);
    if (!socket.valid()) throw std::runtime_error("local Go server unavailable");
    // Keep reads bounded even if the server stops responding.
    socket.setNonBlocking(true);
    std::string auth = "photon:" + password;
    const auto encoded = crypto::base64Encode(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(auth.data()), auth.size()));
    std::string wire = std::string(method) + " " + path + " HTTP/1.0\r\nHost: localhost\r\nAuthorization: Basic " +
                       encoded + "\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n" + body;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::size_t sent = 0;
    std::string response;
    char bytes[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        if (sent < wire.size()) {
            int n = socket.sendSome(wire.data() + sent, wire.size() - sent);
            if (n < 0) throw std::runtime_error("adapter write failed");
            sent += static_cast<std::size_t>(n);
        } else {
            int n = socket.recvSome(bytes, sizeof(bytes));
            if (n < 0) break;
            response.append(bytes, static_cast<std::size_t>(n));
            if (response.size() > 1024 * 1024) throw std::runtime_error("adapter reply too large");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (response.size() < 12 || response.substr(9, 1) != "2") throw std::runtime_error("adapter HTTP failed");
    const auto split = response.find("\r\n\r\n");
    return split == std::string::npos ? "" : response.substr(split + 4);
}
}
int main(int argc, char** argv) {
    photon::net::Guard guard;
    std::uint16_t port = 8080;
    std::string password;
    if (const char* env = std::getenv("PHOTON_PASSWORD")) password = env;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "--http-port") port = static_cast<std::uint16_t>(std::stoi(argv[i + 1]));
        else if (std::string(argv[i]) == "--password") password = argv[i + 1];
        else return 2;
    }
    photon::pcan::Device device;
    bool anchored = false;
    std::uint64_t hw = 0, wall = 0;
    std::uint32_t seq = 0;
    auto lastPoll = std::chrono::steady_clock::time_point{};
    for (;;) {
        try {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPoll >= std::chrono::milliseconds(100)) {
                auto raw = request(port, password, "GET", "/api/pcan/control", "");
                auto root = photon::json::parse(raw);
                if (root) for (const auto& command : (*root)["commands"].asArray()) {
                    auto type = command["type"].asString();
                    if (type == "pcan_config") {
                        device.close(); anchored = false;
                        photon::pcan::Config cfg;
                        cfg.channel = static_cast<int>(command["channel"].asInt());
                        cfg.baud = static_cast<int>(command["baud"].asInt());
                        cfg.listenOnly = command["listen_only"].asBool(true);
                        std::string error;
                        auto opened = photon::pcan::Device::open(cfg, error);
                        if (!opened) {
                            photon::json::Object status; status.emplace("error", error);
                            request(port, password, "POST", "/api/pcan/status",
                                    photon::json::stringify(photon::json::Value(std::move(status))));
                        } else {
                            device = std::move(*opened);
                            request(port, password, "POST", "/api/pcan/status", "{\"available\":true}");
                        }
                    } else if (type == "pcan_close") {
                        device.close(); anchored = false;
                        request(port, password, "POST", "/api/pcan/status", "{\"available\":false}");
                    } else if (type == "pcan_tx" && device.valid() && !device.config().listenOnly) {
                        photon::pcan::Msg msg;
                        msg.id = static_cast<std::uint32_t>(command["can_id"].asInt());
                        msg.len = static_cast<std::uint8_t>(command["dlc"].asInt());
                        msg.msgType = command["flags"].asInt() & 1 ? 2 : 0;
                        auto data = command["data"].asString();
                        if (msg.len > 8 || data.size() != 16) continue;
                        for (int j = 0; j < 8; ++j) msg.data[j] = static_cast<std::uint8_t>(std::stoul(data.substr(j * 2, 2), nullptr, 16));
                        if (device.write(msg) != photon::pcan::kOk) throw std::runtime_error("PCAN transmit failed");
                    }
                }
                lastPoll = now;
            }
            std::string frames;
            if (device.valid()) for (int i = 0; i < 128; ++i) {
                photon::pcan::Msg msg; photon::pcan::Timestamp timestamp;
                auto status = device.read(msg, timestamp);
                if (status == photon::pcan::kQrcvEmpty) break;
                if (status != photon::pcan::kOk) throw std::runtime_error("PCAN capture failed");
                if (msg.msgType & 0x81) continue;
                if (!anchored) {
                    hw = photon::pcan::hardwareTimestampMs(timestamp);
                    wall = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                    anchored = true;
                }
                photon::canp::Batch batch; batch.count = 1; batch.seq = seq++;
                batch.timestamp = photon::pcan::toUnixMs(timestamp, wall, hw);
                batch.packets[0].canId = msg.id | ((msg.msgType & 2) ? 0x80000000u : 0);
                batch.packets[0].dlc = msg.len;
                std::memcpy(batch.packets[0].data, msg.data, 8);
                auto packet = photon::canp::encodeBatch(batch);
                frames.append(reinterpret_cast<const char*>(packet.data()), packet.size());
            }
            if (!frames.empty()) {
                const auto path = "/api/pcan/frames?channel=" + std::to_string(device.config().channel);
                request(port, password, "POST", path.c_str(), frames);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } catch (const std::exception& error) {
            // Disconnect closes the device and prevents queued transmissions.
            device.close(); anchored = false;
            try { request(port, password, "POST", "/api/pcan/status", "{\"available\":false}"); } catch (...) {}
            std::cerr << error.what() << '\n';
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
