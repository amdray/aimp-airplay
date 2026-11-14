#pragma once
#include <winsock2.h>
#include <stdexcept>

namespace airplay {
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data))
            throw std::runtime_error("WSAStartup failed");
    }
    ~SocketRuntime() { WSACleanup(); }
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};
}
