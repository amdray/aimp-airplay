#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "discovery.h"
#include "mdns_query.h"
#include "socket_runtime.h"
#include <stdexcept>
#include <algorithm>

namespace airplay {
namespace {
bool Cancelled(const void* cookie) {
    return cookie && static_cast<const std::atomic_bool*>(cookie)->load(std::memory_order_relaxed);
}
std::wstring Wide(const char* text) {
    if (!text) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (!size) return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    out.pop_back();
    return out;
}
bool Found(mdnssd_service_t* list, void* cookie, bool*) {
    auto& result = *static_cast<std::vector<Receiver>*>(cookie);
    try {
        for (auto* service = list; service; service = service->next) {
            if (service->expired || !service->name || !service->port || !service->addr.s_addr) continue;
            auto id = Wide(service->name);
            auto name = id;
            const auto at = name.find(L'@');
            if (at != std::wstring::npos) name.erase(0, at + 1);
            const auto suffix = name.find(L"._raop._tcp");
            if (suffix != std::wstring::npos) name.erase(suffix);
            char address[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &service->addr, address, sizeof(address));
            Receiver receiver{L"mdns:" + id, name, Wide(address), service->port};
            for (int i = 0; i < service->attr_count; ++i) {
                auto* key = service->attr[i].name;
                auto* value = service->attr[i].value;
                if (!key || !value) continue;
                if (!strcmp(key, "et")) receiver.encryption = value;
                if (!strcmp(key, "am")) receiver.model = value;
                if (!strcmp(key, "md")) receiver.metadata = value;
                if (!strcmp(key, "pw")) receiver.passwordRequired = !_stricmp(value, "true");
                if (!strcmp(key, "ft") || !strcmp(key, "features")) receiver.features = value;
                if (!strcmp(key, "sf") || !strcmp(key, "flags")) receiver.pairingFlags = value;
            }
            auto existing = std::find_if(result.begin(), result.end(), [&](const Receiver& r) { return r.id == receiver.id; });
            if (existing == result.end()) result.push_back(std::move(receiver));
            else *existing = std::move(receiver);
        }
    } catch (...) { return false; }
    return true;
}
}
std::vector<Receiver> DiscoverReceivers(int seconds, const std::atomic_bool* cancelled) {
    if (Cancelled(cancelled)) return {};
    SocketRuntime sockets;
    ULONG size = 16384;
    std::vector<uint8_t> adapters(size);
    auto code = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapters.data()), &size);
    if (code == ERROR_BUFFER_OVERFLOW) {
        adapters.resize(size);
        code = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapters.data()), &size);
    }
    if (code != NO_ERROR) throw std::runtime_error("Cannot enumerate network interfaces");
    std::vector<in_addr> preferred, fallback;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapters.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL) continue;
        for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
            if (address->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto ip = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr)->sin_addr;
            (adapter->FirstGatewayAddress ? preferred : fallback).push_back(ip);
        }
    }
    auto interfaces = preferred;
    interfaces.insert(interfaces.end(), fallback.begin(), fallback.end());
    if (interfaces.empty()) throw std::runtime_error("No active IPv4 interface");
    std::vector<Receiver> result;
    bool success = false;
    for (auto local : interfaces) {
        if (Cancelled(cancelled)) break;
        auto* browser = mdnssd_init(0, local, false);
        if (!browser) continue;
        // Ask for unicast replies; works with Windows binding and shared mDNS ports.
        success |= mdnssd_query_cancelable(browser, "_raop._tcp.local", true, seconds, Found, &result, Cancelled, cancelled);
        mdnssd_close(browser);
    }
    if (!success && !Cancelled(cancelled)) throw std::runtime_error("mDNS query failed");
    return result;
}
}
