#pragma once

#include <SharkSDR/export.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace SharkSDR::net {

// Minimal blocking TCP client socket (Winsock / BSD sockets).
class SHARKSDR_API TcpSocket {
public:
    TcpSocket();
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool connect(const std::string& host, int port, int timeoutMs, std::string* error = nullptr);
    void close();
    bool isOpen() const { return fd_ != kInvalid; }

    // Sends everything; returns false on error.
    bool sendAll(const void* data, size_t size);

    // Waits up to timeoutMs for data. Returns bytes received, 0 on timeout,
    // -1 when the connection was closed or failed.
    int receive(void* buffer, size_t size, int timeoutMs);

    // Receives exactly `size` bytes (false on error/timeout).
    bool receiveAll(void* buffer, size_t size, int timeoutMs);

    void setReceiveBufferSize(int bytes);

private:
    static constexpr intptr_t kInvalid = -1;
    intptr_t fd_ = kInvalid;
};

}  // namespace SharkSDR::net
