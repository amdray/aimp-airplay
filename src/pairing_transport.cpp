#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "pairing_transport.h"
#include "socket_runtime.h"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace airplay::pairing {
namespace {
using Bytes = std::vector<unsigned char>;
void Require(bool valid, const char* error = "Некорректный ответ Apple TV") {
    if (!valid) throw std::runtime_error(error);
}
}
struct Transport::Impl {
    SocketRuntime sockets;
    std::wstring host;
    unsigned short port;
    SOCKET socket = INVALID_SOCKET;
    std::string incoming;
    std::atomic<bool> cancelled{false};
    Impl(std::wstring host, unsigned short port) : host(std::move(host)), port(port) {}
    ~Impl() { if (socket != INVALID_SOCKET) closesocket(socket); }
    void Wait(bool write, ULONGLONG deadline) {
        for (;;) {
            Require(!cancelled.load(), "Привязка отменена");
            Require(GetTickCount64() < deadline, "Apple TV не ответила на запрос привязки");
            fd_set set; FD_ZERO(&set); FD_SET(socket, &set); timeval timeout{0, 50000};
            int result = select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &timeout);
            Require(result != SOCKET_ERROR, "Ошибка соединения с Apple TV"); if (result) return;
        }
    }
    void Connect() {
        sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(port);
        Require(InetPtonW(AF_INET, host.c_str(), &peer.sin_addr) == 1, "Не удалось определить адрес Apple TV");
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); Require(socket != INVALID_SOCKET, "Не удалось открыть соединение с Apple TV");
        u_long nonblock = 1; Require(ioctlsocket(socket, FIONBIO, &nonblock) == 0);
        if (::connect(socket, reinterpret_cast<sockaddr*>(&peer), sizeof(peer))) {
            Require(WSAGetLastError() == WSAEWOULDBLOCK, "Не удалось подключиться к Apple TV");
            Wait(true, GetTickCount64() + 3000); int error = 0, size = sizeof(error);
            Require(getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 && !error, "Не удалось подключиться к Apple TV");
        }
    }
    Bytes Post(const char* path, const Bytes& body) {
        auto deadline = GetTickCount64() + 3000;
        std::string request = std::string("POST ") + path + " HTTP/1.1\r\nUser-Agent: AirPlay/320.20\r\nConnection: keep-alive\r\nContent-Type: " +
            (body.empty() ? "application/octet-stream" : "application/x-apple-binary-plist") + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        Bytes outgoing(request.begin(), request.end()); outgoing.insert(outgoing.end(), body.begin(), body.end());
        for (size_t p = 0; p < outgoing.size();) {
            Wait(true, deadline); int n = send(socket, reinterpret_cast<const char*>(outgoing.data() + p), int(outgoing.size() - p), 0);
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            Require(n > 0, "Apple TV закрыла соединение"); p += size_t(n);
        }
        size_t headerEnd;
        while ((headerEnd = incoming.find("\r\n\r\n")) == std::string::npos) {
            Require(incoming.size() < 8192);
            Wait(false, deadline); char buffer[2048]; int n = recv(socket, buffer, sizeof(buffer), 0);
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            Require(n > 0, "Apple TV закрыла соединение"); incoming.append(buffer, size_t(n));
        }
        Require(headerEnd + 4 <= 8192);
        std::string headers = incoming.substr(0, headerEnd + 4);
        incoming.erase(0, headerEnd + 4);
        size_t size = 0;
        auto space = headers.find(' '); Require(space != std::string::npos && headers.size() > space + 4);
        if (headers.substr(space + 1, 3) != "200") throw std::runtime_error("Apple TV отклонила код или запрос привязки");
        std::string lower = headers; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        Require(lower.find("transfer-encoding:") == std::string::npos);
        size_t p = lower.find("\r\ncontent-length:");
        if (p != std::string::npos) {
            p += 17; while (p < lower.size() && lower[p] == ' ') ++p;
            size_t start = p;
            while (p < lower.size() && lower[p] >= '0' && lower[p] <= '9') { size = size * 10 + (lower[p++] - '0'); Require(size <= 8192); }
            Require(p > start && lower.compare(p, 2, "\r\n") == 0);
        }
        Bytes result(size);
        size_t buffered = std::min(size, incoming.size());
        std::copy_n(incoming.begin(), buffered, result.begin());
        incoming.erase(0, buffered);
        for (size_t p = buffered; p < size;) {
            Wait(false, deadline); int n = recv(socket, reinterpret_cast<char*>(result.data() + p), int(size - p), 0);
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            Require(n > 0, "Apple TV закрыла соединение"); p += size_t(n);
        }
        return result;
    }
};
Transport::Transport(std::wstring host, unsigned short port) : impl_(std::make_unique<Impl>(std::move(host), port)) {}
Transport::~Transport() = default;
void Transport::Connect() { impl_->Connect(); }
std::vector<unsigned char> Transport::Post(const char* path, const std::vector<unsigned char>& body) { return impl_->Post(path, body); }
void Transport::Cancel() noexcept { impl_->cancelled = true; }
}
