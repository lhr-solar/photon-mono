#include "photon/websocket.hpp"

#include "photon/endian.hpp"
#include "photon/ws_handshake.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace photon::ws {
using photon::readU16Be;
using photon::readU64Be;
using photon::writeBytes;
using photon::writeU16Be;
using photon::writeU64Be;

namespace {

constexpr std::size_t kMaxPayload = 4 * 1024 * 1024;

std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

bool headerHasToken(std::string_view value, std::string_view token) {
    std::string lower = toLower(std::string(value));
    std::size_t start = 0;
    while (start < lower.size()) {
        const std::size_t comma = lower.find(',', start);
        const std::string_view part = trim(std::string_view(lower).substr(start, comma - start));
        if (part == token) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

void unmask(std::uint8_t* data, std::size_t len, const std::uint8_t key[4]) {
    for (std::size_t i = 0; i < len; ++i) {
        data[i] ^= key[i & 3];
    }
}

}  // namespace

std::vector<std::uint8_t> encodeServerFrame(Opcode opcode, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(0x80 | (static_cast<std::uint8_t>(opcode) & 0x0F)));
    const std::size_t n = payload.size();
    if (n <= 125) {
        out.push_back(static_cast<std::uint8_t>(n));  // unmasked
    } else if (n <= 0xFFFF) {
        out.push_back(126);
        writeU16Be(out, static_cast<std::uint16_t>(n));
    } else {
        out.push_back(127);
        writeU64Be(out, static_cast<std::uint64_t>(n));
    }
    writeBytes(out, payload);
    return out;
}

Connection::Connection(net::Socket socket) : socket_(std::move(socket)), peer_(socket_.peerLabel()) {}

HandshakeStatus Connection::readHandshake() {
    if (handshakeOk_) {
        return HandshakeStatus::Ok;
    }
    std::uint8_t buf[1024];
    const int n = socket_.recvSome(buf, sizeof(buf));
    if (n < 0) {
        return HandshakeStatus::BadRequest;
    }
    if (n > 0) {
        inq_.insert(inq_.end(), buf, buf + n);
    }
    const std::string_view text(reinterpret_cast<const char*>(inq_.data()), inq_.size());
    const auto end = text.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        if (inq_.size() > 16 * 1024) {
            return HandshakeStatus::BadRequest;
        }
        return HandshakeStatus::Incomplete;
    }

    const std::string headers(text.substr(0, end));
    bool upgrade = false;
    bool connection = false;
    bool version13 = false;
    std::string key;
    std::size_t lineStart = 0;
    while (lineStart < headers.size()) {
        auto lineEnd = headers.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = headers.size();
        }
        const std::string_view line = std::string_view(headers).substr(lineStart, lineEnd - lineStart);
        if (lineStart == 0) {
            if (line.find("GET ") != 0) {
                return HandshakeStatus::BadRequest;
            }
        } else if (!line.empty()) {
            const auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                const std::string name = toLower(std::string(trim(line.substr(0, colon))));
                const std::string value(trim(line.substr(colon + 1)));
                if (name == "upgrade") {
                    upgrade = toLower(value) == "websocket";
                } else if (name == "connection") {
                    connection = headerHasToken(value, "upgrade");
                } else if (name == "sec-websocket-key") {
                    key = value;
                } else if (name == "sec-websocket-version") {
                    version13 = value == "13";
                }
            }
        }
        if (lineEnd == headers.size()) {
            break;
        }
        lineStart = lineEnd + 2;
    }

    if (!upgrade || !connection || !version13 || key.empty()) {
        return HandshakeStatus::BadRequest;
    }
    clientKey_ = std::move(key);
    inq_.erase(inq_.begin(), inq_.begin() + static_cast<std::ptrdiff_t>(end + 4));
    return HandshakeStatus::Ok;
}

bool Connection::writeHandshakeResponse() {
    const std::string accept = acceptKey(clientKey_);
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: " +
                                 accept + "\r\n\r\n";
    outq_.insert(outq_.end(), response.begin(), response.end());
    handshakeOk_ = true;
    return flushOutbound();
}

void Connection::queueFrame(Opcode opcode, std::span<const std::uint8_t> payload) {
    const auto frame = encodeServerFrame(opcode, payload);
    outq_.insert(outq_.end(), frame.begin(), frame.end());
}

void Connection::queueText(std::string_view text) {
    queueFrame(Opcode::Text, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void Connection::queueBinary(std::span<const std::uint8_t> payload) {
    queueFrame(Opcode::Binary, payload);
}

void Connection::queuePing(std::span<const std::uint8_t> payload) {
    queueFrame(Opcode::Ping, payload);
}

void Connection::queuePong(std::span<const std::uint8_t> payload) {
    queueFrame(Opcode::Pong, payload);
}

void Connection::queueClose(std::uint16_t code, std::string_view reason) {
    std::vector<std::uint8_t> payload;
    writeU16Be(payload, code);
    payload.insert(payload.end(), reason.begin(), reason.end());
    queueFrame(Opcode::Close, payload);
    closeSent_ = true;
}

bool Connection::readAvailable() {
    std::uint8_t buf[4096];
    for (;;) {
        const int n = socket_.recvSome(buf, sizeof(buf));
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        inq_.insert(inq_.end(), buf, buf + n);
        if (inq_.size() > kMaxPayload + 16) {
            return false;
        }
    }
}

bool Connection::flushOutbound() {
    while (!outq_.empty()) {
        const int n = socket_.sendSome(outq_.data(), outq_.size());
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        outq_.erase(outq_.begin(), outq_.begin() + n);
    }
    return true;
}

std::optional<Frame> Connection::takeFrame() {
    auto frame = parseOneFrame();
    if (!frame) {
        return std::nullopt;
    }
    if (frame->opcode == Opcode::Close) {
        closeReceived_ = true;
        if (!closeSent_) {
            queueClose(1000, "bye");
        }
        return frame;
    }
    if (frame->opcode == Opcode::Ping) {
        queuePong(frame->payload);
        return frame;
    }
    if (frame->opcode == Opcode::Pong) {
        return frame;
    }
    if (!frame->fin) {
        if (frame->opcode != Opcode::Continuation) {
            fragmentOpcode_ = frame->opcode;
            fragment_ = std::move(frame->payload);
        } else {
            fragment_.insert(fragment_.end(), frame->payload.begin(), frame->payload.end());
        }
        return std::nullopt;
    }
    if (frame->opcode == Opcode::Continuation) {
        fragment_.insert(fragment_.end(), frame->payload.begin(), frame->payload.end());
        Frame complete;
        complete.opcode = fragmentOpcode_;
        complete.payload = std::move(fragment_);
        fragment_.clear();
        return complete;
    }
    return frame;
}

std::optional<Frame> Connection::parseOneFrame() {
    if (inq_.size() < 2) {
        return std::nullopt;
    }
    const std::uint8_t b0 = inq_[0];
    const std::uint8_t b1 = inq_[1];
    const bool fin = (b0 & 0x80) != 0;
    const std::uint8_t opcode = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    std::uint64_t payloadLen = b1 & 0x7F;
    std::size_t headerLen = 2;
    if (payloadLen == 126) {
        if (inq_.size() < 4) {
            return std::nullopt;
        }
        std::size_t off = 2;
        std::uint16_t n16 = 0;
        readU16Be(inq_, off, n16);
        payloadLen = n16;
        headerLen = 4;
    } else if (payloadLen == 127) {
        if (inq_.size() < 10) {
            return std::nullopt;
        }
        std::size_t off = 2;
        readU64Be(inq_, off, payloadLen);
        headerLen = 10;
    }
    if (!masked) {
        // Client-to-server frames must be masked (RFC 6455 §5.1).
        inq_.clear();
        return std::nullopt;
    }
    headerLen += 4;
    if (payloadLen > kMaxPayload) {
        inq_.clear();
        return std::nullopt;
    }
    if (inq_.size() < headerLen + static_cast<std::size_t>(payloadLen)) {
        return std::nullopt;
    }
    std::uint8_t mask[4];
    std::memcpy(mask, inq_.data() + headerLen - 4, 4);
    Frame frame;
    frame.fin = fin;
    frame.opcode = static_cast<Opcode>(opcode);
    frame.payload.assign(inq_.begin() + static_cast<std::ptrdiff_t>(headerLen),
                         inq_.begin() + static_cast<std::ptrdiff_t>(headerLen + payloadLen));
    unmask(frame.payload.data(), frame.payload.size(), mask);
    inq_.erase(inq_.begin(), inq_.begin() + static_cast<std::ptrdiff_t>(headerLen + payloadLen));
    return frame;
}

}  // namespace photon::ws
