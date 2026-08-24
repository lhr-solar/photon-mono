#include "photon/net.hpp"

#include <cerrno>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace photon::net {
namespace {

int lastError() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool wouldBlock(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS;
#endif
}

void closeNative(Native s) {
    if (s == kInvalid) {
        return;
    }
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

Socket makeConnected(std::string_view host, std::uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (passive) {
        hints.ai_flags = AI_PASSIVE;
    }

    const std::string hostStr(host);
    const std::string portStr = std::to_string(port);
    addrinfo* result = nullptr;
    if (getaddrinfo(hostStr.empty() ? nullptr : hostStr.c_str(), portStr.c_str(), &hints, &result) != 0) {
        return {};
    }

    Socket sock;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        Socket candidate(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!candidate.valid()) {
            continue;
        }
        candidate.setReuseAddr(true);
        candidate.setNoDelay(true);
        if (passive) {
            if (bind(candidate.native(), ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != 0) {
                continue;
            }
            if (listen(candidate.native(), 16) != 0) {
                continue;
            }
            sock = std::move(candidate);
            break;
        }
        const int rc = connect(candidate.native(), ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc == 0 || wouldBlock(lastError())) {
            sock = std::move(candidate);
            break;
        }
    }
    freeaddrinfo(result);
    return sock;
}

}  // namespace

Guard::Guard() {
#if defined(_WIN32)
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

Guard::~Guard() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

Socket::Socket(Native native) : native_(native) {}

Socket::Socket(Socket&& other) noexcept : native_(other.native_) {
    other.native_ = kInvalid;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        native_ = other.native_;
        other.native_ = kInvalid;
    }
    return *this;
}

Socket::~Socket() {
    close();
}

Native Socket::release() {
    const Native s = native_;
    native_ = kInvalid;
    return s;
}

void Socket::close() {
    closeNative(native_);
    native_ = kInvalid;
}

bool Socket::setNonBlocking(bool enabled) const {
    if (!valid()) {
        return false;
    }
#if defined(_WIN32)
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(native_, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(native_, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    if (enabled) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(native_, F_SETFL, flags) == 0;
#endif
}

bool Socket::setNoDelay(bool enabled) const {
    if (!valid()) {
        return false;
    }
    const int flag = enabled ? 1 : 0;
    return setsockopt(native_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag)) == 0;
}

bool Socket::setReuseAddr(bool enabled) const {
    if (!valid()) {
        return false;
    }
    const int flag = enabled ? 1 : 0;
    return setsockopt(native_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&flag), sizeof(flag)) == 0;
}

int Socket::sendSome(const void* data, std::size_t len) const {
    if (!valid() || len == 0) {
        return 0;
    }
#if defined(_WIN32)
    const int n = ::send(native_, static_cast<const char*>(data), static_cast<int>(len), 0);
#else
    const ssize_t n = ::send(native_, data, len, 0);
#endif
    if (n < 0) {
        return wouldBlock(lastError()) ? 0 : -1;
    }
    return static_cast<int>(n);
}

int Socket::recvSome(void* data, std::size_t len) const {
    if (!valid() || len == 0) {
        return 0;
    }
#if defined(_WIN32)
    const int n = ::recv(native_, static_cast<char*>(data), static_cast<int>(len), 0);
#else
    const ssize_t n = ::recv(native_, data, len, 0);
#endif
    if (n < 0) {
        return wouldBlock(lastError()) ? 0 : -1;
    }
    if (n == 0) {
        return -1;
    }
    return static_cast<int>(n);
}

std::string Socket::peerLabel() const {
    if (!valid()) {
        return "closed";
    }
    sockaddr_in addr{};
#if defined(_WIN32)
    int len = static_cast<int>(sizeof(addr));
#else
    socklen_t len = sizeof(addr);
#endif
    if (getpeername(native_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return "unknown";
    }
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

int poll(PollFd* fds, std::size_t count, int timeoutMs) {
    if (count == 0) {
#if defined(_WIN32)
        Sleep(timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0);
#else
        if (timeoutMs > 0) {
            usleep(static_cast<useconds_t>(timeoutMs) * 1000);
        }
#endif
        return 0;
    }
#if defined(_WIN32)
    std::vector<WSAPOLLFD> native(count);
    for (std::size_t i = 0; i < count; ++i) {
        native[i].fd = fds[i].fd;
        native[i].events = fds[i].events;
        native[i].revents = 0;
    }
    const int rc = WSAPoll(native.data(), static_cast<ULONG>(count), timeoutMs);
    for (std::size_t i = 0; i < count; ++i) {
        fds[i].revents = native[i].revents;
    }
    return rc;
#else
    std::vector<pollfd> native(count);
    for (std::size_t i = 0; i < count; ++i) {
        native[i].fd = fds[i].fd;
        native[i].events = fds[i].events;
        native[i].revents = 0;
    }
    const int rc = ::poll(native.data(), static_cast<nfds_t>(count), timeoutMs);
    for (std::size_t i = 0; i < count; ++i) {
        fds[i].revents = native[i].revents;
    }
    return rc;
#endif
}

Socket listenLoopback(std::uint16_t port) {
    Socket sock = makeConnected("127.0.0.1", port, true);
    if (sock.valid()) {
        sock.setNonBlocking(true);
    }
    return sock;
}

Socket acceptClient(const Socket& listener) {
    if (!listener.valid()) {
        return {};
    }
    sockaddr_in addr{};
#if defined(_WIN32)
    int len = static_cast<int>(sizeof(addr));
    Socket client(::accept(listener.native(), reinterpret_cast<sockaddr*>(&addr), &len));
#else
    socklen_t len = sizeof(addr);
    Socket client(::accept(listener.native(), reinterpret_cast<sockaddr*>(&addr), &len));
#endif
    if (!client.valid()) {
        return {};
    }
    client.setNonBlocking(true);
    client.setNoDelay(true);
    return client;
}

Socket connectTcp(std::string_view host, std::uint16_t port) {
    Socket sock = makeConnected(host, port, false);
    if (sock.valid()) {
        sock.setNonBlocking(true);
        sock.setNoDelay(true);
    }
    return sock;
}

}  // namespace photon::net
