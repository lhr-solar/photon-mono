#pragma once

#include "photon/net.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ws {

enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Frame {
    Opcode opcode = Opcode::Binary;
    bool fin = true;
    std::vector<std::uint8_t> payload;
};

enum class HandshakeStatus {
    Incomplete,
    Ok,
    BadRequest,
    HttpExport,
};

class Connection {
public:
    explicit Connection(net::Socket socket);

    net::Socket& socket() { return socket_; }
    const net::Socket& socket() const { return socket_; }
    bool handshakeComplete() const { return handshakeOk_; }
    bool wantsClose() const { return closeSent_ || closeReceived_; }
    const std::string& peer() const { return peer_; }

    HandshakeStatus readHandshake();
    bool writeHandshakeResponse();
    const std::string& httpTarget() const { return httpTarget_; }
    net::Socket releaseSocket() { return std::move(socket_); }
    void setClientMode(bool on) { clientMode_ = on; }
    bool beginClientHandshake(std::string_view path, std::string_view host, std::string_view basicB64);
    HandshakeStatus readClientHandshake();

    void queueFrame(Opcode opcode, std::span<const std::uint8_t> payload);
    void queueText(std::string_view text);
    void queueBinary(std::span<const std::uint8_t> payload);
    void queuePing(std::span<const std::uint8_t> payload = {});
    void queuePong(std::span<const std::uint8_t> payload = {});
    void queueClose(std::uint16_t code = 1000, std::string_view reason = {});

    // Pull complete application frames (text/binary/ping/pong/close). Control
    // frames are already unmasked. Continuation is reassembled.
    std::optional<Frame> takeFrame();

    bool readAvailable();
    bool flushOutbound();
    bool hasOutbound() const { return !outq_.empty(); }
    std::size_t outboundBytes() const { return outq_.size(); }

private:
    net::Socket socket_;
    std::string peer_;
    std::vector<std::uint8_t> inq_;
    std::vector<std::uint8_t> outq_;
    std::string clientKey_;
    std::string httpTarget_;
    std::vector<std::uint8_t> fragment_;
    Opcode fragmentOpcode_ = Opcode::Binary;
    bool handshakeOk_ = false;
    bool clientMode_ = false;
    bool closeSent_ = false;
    bool closeReceived_ = false;

    std::optional<Frame> parseOneFrame();
};

std::vector<std::uint8_t> encodeServerFrame(Opcode opcode, std::span<const std::uint8_t> payload);
std::vector<std::uint8_t> encodeClientFrame(Opcode opcode, std::span<const std::uint8_t> payload,
                                            const std::uint8_t mask[4]);

}  // namespace photon::ws
