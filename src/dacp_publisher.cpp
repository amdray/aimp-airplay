#include <ws2tcpip.h>
#include <windows.h>
#include "dacp_publisher.h"
#include "dacp_socket.h"
#include "dacp_dns.h"
namespace airplay::dacp {
struct Publisher::Impl {
    std::string id;
    unsigned short port;
    RemoteControl::ErrorCallback errorCallback;
    uint32_t version = 0;
    in_addr peer{}, local{};
    std::unique_ptr<Socket> mdns;
    Bytes advertisement;
    ULONGLONG advertised = 0, publishAttempt = 0;
    sockaddr_in multicast{};
    void ReportError(const std::string& stage, const std::string& detail) noexcept {
        try { if (errorCallback) errorCallback(stage, detail); } catch (...) {}
    }
    Impl(std::string id, unsigned short port, RemoteControl::ErrorCallback error)
        : id(std::move(id)), port(port), errorCallback(std::move(error)) {
        multicast.sin_family = AF_INET; multicast.sin_port = htons(5353);
        InetPtonA(AF_INET, "224.0.0.251", &multicast.sin_addr);
    }
    void Poll(const std::wstring& target, uint32_t next) {
        if (version != next || (!mdns && !target.empty() && GetTickCount64() - publishAttempt >= 5000)) {
            publishAttempt = GetTickCount64();
            version = next; mdns.reset(); peer = {}; local = {}; advertisement.clear(); advertised = 0;
            if (!target.empty() && InetPtonW(AF_INET, target.c_str(), &peer) == 1) {
                Socket route; sockaddr_in destination{}; destination.sin_family = AF_INET; destination.sin_addr = peer; destination.sin_port = htons(7000);
                sockaddr_in source{}; int size = sizeof(source);
                if (route.Open(SOCK_DGRAM) && !connect(route.value, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) && !getsockname(route.value, reinterpret_cast<sockaddr*>(&source), &size)) {
                    local = source.sin_addr; auto candidate = std::make_unique<Socket>(); BOOL reuse = TRUE;
                    sockaddr_in bindAddress{}; bindAddress.sin_family = AF_INET; bindAddress.sin_port = htons(5353);
                    ip_mreq membership{multicast.sin_addr, local}; DWORD ttl = 255;
                    if (candidate->Open(SOCK_DGRAM) && !setsockopt(candidate->value, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse)) &&
                        !bind(candidate->value, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) &&
                        !setsockopt(candidate->value, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<char*>(&membership), sizeof(membership)) &&
                        !setsockopt(candidate->value, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<char*>(&local), sizeof(local)) &&
                        !setsockopt(candidate->value, IPPROTO_IP, IP_TTL, reinterpret_cast<char*>(&ttl), sizeof(ttl))) {
                        setsockopt(candidate->value, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<char*>(&ttl), sizeof(ttl));
                        candidate->Nonblock(); mdns = std::move(candidate); advertisement = Advertisement(id, port, local);
                    }
                }
            }
        }
        if (mdns) {
            bool announce = !advertised || GetTickCount64() - advertised >= 30000;
            unsigned char bytes[4096]; sockaddr_in from{}; int size = sizeof(from);
            int n = recvfrom(mdns->value, reinterpret_cast<char*>(bytes), sizeof(bytes), 0, reinterpret_cast<sockaddr*>(&from), &size);
            if (n > 0 && from.sin_addr.s_addr == peer.s_addr) {
                bool unicast = false;
                bool relevant = RelevantQuery(bytes, size_t(n), id, &unicast);
                if (relevant && unicast) {
                    auto sent = sendto(mdns->value, reinterpret_cast<const char*>(advertisement.data()), int(advertisement.size()), 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
                    if (sent != int(advertisement.size())) ReportError("mdns_unicast", std::to_string(WSAGetLastError()));
                } else if (relevant) announce = true;
            }
            if (announce) {
                auto sent = sendto(mdns->value, reinterpret_cast<const char*>(advertisement.data()), int(advertisement.size()), 0, reinterpret_cast<sockaddr*>(&multicast), sizeof(multicast));
                if (sent != int(advertisement.size())) ReportError("mdns_multicast", std::to_string(WSAGetLastError()));
                advertised = GetTickCount64();
            }
        }
    }
};
Publisher::Publisher(std::string id, unsigned short port, RemoteControl::ErrorCallback error)
    : impl_(std::make_unique<Impl>(std::move(id), port, std::move(error))) {}
Publisher::~Publisher() = default;
void Publisher::Poll(const std::wstring& target, uint32_t generation) { impl_->Poll(target, generation); }
in_addr Publisher::Peer() const { return impl_->peer; }
}
