#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <poll.h>
#endif

namespace photon::net {

#if defined(_WIN32)
using Native = SOCKET;
inline constexpr Native kInvalid = INVALID_SOCKET;
inline constexpr short kPollIn = POLLRDNORM;
inline constexpr short kPollOut = POLLWRNORM;
inline constexpr short kPollErr = POLLERR;
inline constexpr short kPollHup = POLLHUP;
#else
using Native = int;
inline constexpr Native kInvalid = -1;
inline constexpr short kPollIn = POLLIN;
inline constexpr short kPollOut = POLLOUT;
inline constexpr short kPollErr = POLLERR;
inline constexpr short kPollHup = POLLHUP;
#endif

class Guard {
public:
    Guard();
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

class Socket {
public:
    Socket() = default;
    explicit Socket(Native native);
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const { return native_ != kInvalid; }
    Native native() const { return native_; }
    Native release();
    void close();

    bool setNonBlocking(bool enabled) const;
    bool setNoDelay(bool enabled) const;
    bool setReuseAddr(bool enabled) const;

    // Returns bytes sent, 0 if would-block, -1 on error / peer close for recv.
    int sendSome(const void* data, std::size_t len) const;
    int recvSome(void* data, std::size_t len) const;

    std::string peerLabel() const;

private:
    Native native_ = kInvalid;
};

struct PollFd {
    Native fd = kInvalid;
    short events = 0;
    short revents = 0;
};

int poll(PollFd* fds, std::size_t count, int timeoutMs);

Socket listenLoopback(std::uint16_t port);
Socket acceptClient(const Socket& listener);
Socket connectTcp(std::string_view host, std::uint16_t port);

}  // namespace photon::net
