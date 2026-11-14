#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include "remote_control.h"
#include "socket_runtime.h"
#include "dacp_server.h"
#include "dacp_publisher.h"
#include <atomic>
#include <array>
#include <mutex>
#include <thread>
namespace airplay {
struct RemoteControl::Impl {
    SocketRuntime sockets;
    std::string id, token;
    Callback callback;
    ErrorCallback errorCallback;
    mutable std::mutex mutex;
    std::wstring host;
    uint32_t generation = 1;
    std::atomic<bool> stopping{false};
    std::unique_ptr<dacp::Server> server;
    std::thread worker;
    void ReportError(const std::string& stage, const std::string& detail) noexcept {
        try { if (errorCallback) errorCallback(stage, detail); } catch (...) {}
    }
    explicit Impl(Callback cb, ErrorCallback error) : callback(std::move(cb)), errorCallback(std::move(error)) {
        std::array<unsigned char, 12> random{};
        if (BCryptGenRandom(nullptr, random.data(), ULONG(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) throw std::runtime_error("DACP random generation failed");
        const char* hex = "0123456789ABCDEF";
        for (size_t i = 0; i < 8; ++i) { id += hex[random[i] >> 4]; id += hex[random[i] & 15]; }
        uint32_t number = 0; for (size_t i = 8; i < 12; ++i) number = (number << 8) | random[i]; token = std::to_string(number);
        server = std::make_unique<dacp::Server>(token, stopping,
            [this](const RemoteCommand* command, uint32_t version) {
                std::lock_guard<std::mutex> lock(mutex);
                if (version != generation || host.empty()) return dacp::Server::DispatchResult::Rejected;
                if (!command) return dacp::Server::DispatchResult::UnknownCommand;
                callback(*command, version);
                return dacp::Server::DispatchResult::Accepted;
            }, errorCallback);
        worker = std::thread([this] { Run(); });
    }
    ~Impl() { stopping = true; if (worker.joinable()) worker.join(); }
    void Run() noexcept {
        try {
            dacp::Publisher publisher(id, server->Port(), errorCallback);
            while (!stopping) {
                std::wstring target; uint32_t version;
                { std::lock_guard<std::mutex> lock(mutex); target = host; version = generation; }
                publisher.Poll(target, version);
                server->Poll(publisher.Peer(), version);
            }
        } catch (const std::exception& error) {
            ReportError("remote_worker", error.what());
        } catch (...) {
            ReportError("remote_worker", "unknown exception");
        }
    }
};
RemoteControl::RemoteControl(Callback callback, ErrorCallback error) : impl_(new Impl(std::move(callback), std::move(error))) {}
RemoteControl::~RemoteControl() = default;
void RemoteControl::Select(const std::wstring& host) { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->host != host) { impl_->host = host; ++impl_->generation; } }
bool RemoteControl::IsCurrent(uint32_t generation) const { std::lock_guard<std::mutex> lock(impl_->mutex); return generation == impl_->generation && !impl_->host.empty(); }
const std::string& RemoteControl::Id() const { return impl_->id; }
const std::string& RemoteControl::Token() const { return impl_->token; }
unsigned short RemoteControl::Port() const { return impl_->server->Port(); }
}
