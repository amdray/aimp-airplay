#include "dacp_server.h"
#include "dacp_socket.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
namespace airplay::dacp {
struct Server::Impl {
    Socket listener;
    unsigned short port = 0;
    std::string token;
    const std::atomic<bool>& stopping;
    Dispatch dispatch;
    RemoteControl::ErrorCallback errorCallback;
    void ReportError(const std::string& stage, const std::string& detail) noexcept {
        try { if (errorCallback) errorCallback(stage, detail); } catch (...) {}
    }
    struct Replies {
        std::deque<std::pair<std::unique_ptr<Socket>, ULONGLONG>> sockets;
        void Retire(Socket& socket) {
            auto reply = std::make_unique<Socket>(); reply->value = socket.value; socket.value = INVALID_SOCKET;
            sockets.emplace_back(std::move(reply), GetTickCount64() + 100);
            if (sockets.size() > 32) sockets.pop_front();
        }
        void Expire() { while (!sockets.empty() && GetTickCount64() >= sockets.front().second) sockets.pop_front(); }
    } replies;
    Impl(std::string token, const std::atomic<bool>& stopping, Dispatch dispatch, RemoteControl::ErrorCallback error)
        : token(std::move(token)), stopping(stopping), dispatch(std::move(dispatch)), errorCallback(std::move(error)) {
        sockaddr_in address{}; address.sin_family = AF_INET;
        if (!listener.Open(SOCK_STREAM) || bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener.value, 4)) throw std::runtime_error("DACP listener failed");
        int size = sizeof(address); if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size)) throw std::runtime_error("DACP listener address failed");
        port = ntohs(address.sin_port); listener.Nonblock();
    }
    bool Wait(SOCKET socket, bool write, ULONGLONG deadline) {
        while (!stopping && GetTickCount64() < deadline) {
            fd_set set; FD_ZERO(&set); FD_SET(socket, &set); timeval wait{0, 50000};
            int n = select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &wait);
            if (n == SOCKET_ERROR) ReportError("socket_wait", std::to_string(WSAGetLastError()));
            if (n) return n > 0;
        }
        return false;
    }
    void Handle(SOCKET socket, uint32_t version) {
        auto deadline = GetTickCount64() + 1000; std::string request;
        while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
            if (!Wait(socket, false, deadline)) return;
            char bytes[1024]; int n = recv(socket, bytes, sizeof(bytes), 0);
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (n <= 0) return;
            request.append(bytes, size_t(n));
        }
        if (request.size() >= 8192 || request.find("\r\n\r\n") == std::string::npos) return;
        auto end = request.find("\r\n"); auto line = request.substr(0, end);
        std::string command; bool authorized = false, duplicate = false;
        if (line.rfind("GET /ctrl-int/1/", 0) == 0) {
            size_t space = line.find(' ', 16);
            if (space != std::string::npos && (line.substr(space) == " HTTP/1.1" || line.substr(space) == " HTTP/1.0")) command = line.substr(16, space - 16);
        }
        size_t p = end + 2;
        while (p < request.size()) {
            end = request.find("\r\n", p); if (end == std::string::npos || end == p) break;
            auto colon = request.find(':', p); if (colon == std::string::npos || colon >= end) return;
            std::string key = request.substr(p, colon - p); std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (key == "active-remote") {
                if (duplicate) return;
                duplicate = true;
                auto value = request.substr(colon + 1, end - colon - 1);
                auto start = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t");
                authorized = start != std::string::npos && value.substr(start, last - start + 1) == token;
            }
            p = end + 2;
        }
        static const std::map<std::string, RemoteCommand> commands{
            {"playpause", RemoteCommand::PlayPause}, {"play", RemoteCommand::Play}, {"playresume", RemoteCommand::Play},
            {"pause", RemoteCommand::Pause}, {"stop", RemoteCommand::Stop}, {"nextitem", RemoteCommand::Next},
            {"previtem", RemoteCommand::Previous}, {"restartitem", RemoteCommand::Restart}};
        auto found = commands.find(command); unsigned code = 403;
        if (authorized) {
            switch (dispatch(found != commands.end() ? &found->second : nullptr, version)) {
            case DispatchResult::Rejected:
                ReportError("remote_request", "receiver selection changed");
                break;
            case DispatchResult::UnknownCommand:
                if (command.size() > 64 || command.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
                    command.clear();
                ReportError("remote_request", command.empty() ? "unrecognized command" : "unrecognized command " + command);
                code = 404;
                break;
            case DispatchResult::Accepted: code = 204; break;
            }
        } else {
            ReportError("remote_request", "invalid or missing Active-Remote");
        }
        std::string reply = "HTTP/1.1 " + std::to_string(code) + (code == 204 ? " No Content" : code == 403 ? " Forbidden" : " Not Found") + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        for (size_t sent = 0; sent < reply.size();) {
            if (!Wait(socket, true, deadline)) return;
            int n = send(socket, reply.data() + sent, int(reply.size() - sent), 0);
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (n <= 0) return;
            sent += size_t(n);
        }
        shutdown(socket, SD_SEND);
    }
    void Poll(in_addr peer, uint32_t version) {
        // Retain completed sockets while queued HTTP replies leave the TCP stack.
        replies.Expire();
        sockaddr_in from{}; int size = sizeof(from); Socket connection;
        if (Wait(listener.value, false, GetTickCount64() + 50)) {
            connection.value = accept(listener.value, reinterpret_cast<sockaddr*>(&from), &size);
            if (connection.value == INVALID_SOCKET) ReportError("tcp_accept", std::to_string(WSAGetLastError()));
            if (connection.value != INVALID_SOCKET && peer.s_addr && from.sin_addr.s_addr == peer.s_addr) {
                connection.Nonblock(); Handle(connection.value, version); replies.Retire(connection);
            }
        }
    }
};
Server::Server(std::string token, const std::atomic<bool>& stopping, Dispatch dispatch, RemoteControl::ErrorCallback error)
    : impl_(std::make_unique<Impl>(std::move(token), stopping, std::move(dispatch), std::move(error))) {}
Server::~Server() = default;
unsigned short Server::Port() const { return impl_->port; }
void Server::Poll(in_addr peer, uint32_t generation) { impl_->Poll(peer, generation); }
}
