#pragma once
#include <winsock2.h>
#include <stdexcept>
namespace airplay::dacp {
struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
    bool Open(int type) { value = socket(AF_INET, type, 0); return value != INVALID_SOCKET; }
    void Nonblock() { u_long n = 1; if (ioctlsocket(value, FIONBIO, &n)) throw std::runtime_error("DACP socket mode failed"); }
};
}
