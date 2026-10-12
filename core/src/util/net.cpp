#include <SharkSDR/util/net.h>

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socklen_t = int;
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0  // macOS: SO_NOSIGPIPE is set on the socket instead
#  endif
#endif

namespace SharkSDR::net {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
const NativeSocket kNativeInvalid = INVALID_SOCKET;
void closeNative(NativeSocket s) { closesocket(s); }
void setNonBlocking(NativeSocket s, bool on) {
    u_long mode = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
}
bool connectInProgress() {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
#else
using NativeSocket = int;
const NativeSocket kNativeInvalid = -1;
void closeNative(NativeSocket s) { ::close(s); }
void setNonBlocking(NativeSocket s, bool on) {
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
bool connectInProgress() { return errno == EINPROGRESS; }
#endif

void initNetworking() {
#if defined(_WIN32)
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

// Waits until the socket is readable (or writable). Returns >0 ready, 0 timeout, <0 error.
int waitSocket(NativeSocket s, bool write, int timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(int(s) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
}

}  // namespace

TcpSocket::TcpSocket() { initNetworking(); }

TcpSocket::~TcpSocket() { close(); }

bool TcpSocket::connect(const std::string& host, int port, int timeoutMs, std::string* error) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0 || !result) {
        if (error) *error = "Cannot resolve host '" + host + "'";
        return false;
    }

    std::string lastError = "Connection failed";
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        NativeSocket s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kNativeInvalid) continue;
        setNonBlocking(s, true);
        int rc = ::connect(s, ai->ai_addr, socklen_t(ai->ai_addrlen));
        bool ok = rc == 0;
        if (!ok && connectInProgress()) {
            if (waitSocket(s, true, timeoutMs) > 0) {
                int soError = 0;
                socklen_t len = sizeof(soError);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
                ok = soError == 0;
                if (!ok) lastError = "Connection refused";
            } else {
                lastError = "Connection timed out";
            }
        }
        if (ok) {
            setNonBlocking(s, false);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#if defined(SO_NOSIGPIPE)
            setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            fd_ = intptr_t(s);
            freeaddrinfo(result);
            return true;
        }
        closeNative(s);
    }
    freeaddrinfo(result);
    if (error) *error = lastError;
    return false;
}

void TcpSocket::close() {
    if (fd_ == kInvalid) return;
    NativeSocket s = NativeSocket(fd_);
#if defined(_WIN32)
    shutdown(s, SD_BOTH);
#else
    shutdown(s, SHUT_RDWR);
#endif
    closeNative(s);
    fd_ = kInvalid;
}

bool TcpSocket::sendAll(const void* data, size_t size) {
    if (fd_ == kInvalid) return false;
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
#if defined(_WIN32)
        int n = ::send(NativeSocket(fd_), p, int(size), 0);
#else
        ssize_t n = ::send(NativeSocket(fd_), p, size, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}

int TcpSocket::receive(void* buffer, size_t size, int timeoutMs) {
    if (fd_ == kInvalid) return -1;
    int ready = waitSocket(NativeSocket(fd_), false, timeoutMs);
    if (ready == 0) return 0;
    if (ready < 0) return -1;
#if defined(_WIN32)
    int n = ::recv(NativeSocket(fd_), static_cast<char*>(buffer), int(size), 0);
#else
    ssize_t n = ::recv(NativeSocket(fd_), buffer, size, 0);
#endif
    return n > 0 ? int(n) : -1;
}

bool TcpSocket::receiveAll(void* buffer, size_t size, int timeoutMs) {
    char* p = static_cast<char*>(buffer);
    while (size > 0) {
        int n = receive(p, size, timeoutMs);
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}

void TcpSocket::setReceiveBufferSize(int bytes) {
    if (fd_ == kInvalid) return;
    setsockopt(NativeSocket(fd_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

}  // namespace SharkSDR::net
