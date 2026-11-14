#include <winsock2.h>
#include <ws2tcpip.h>
#include "raop_session.h"
#include "crypto_runtime.h"
#include "socket_runtime.h"
#include "protocol_diagnostics.h"
#include "receiver_auth.h"
#include "transient_pairing.h"
extern "C" {
#include "raop_client.h"
int rtspcl_pairing_post(struct rtspcl_s*, const char*, const unsigned char*, int, unsigned char*, int);
}
#include <cstring>
#include <stdexcept>

using airplay::detail::protocol;
using airplay::detail::RememberFailure;
namespace {
struct Operation {
    const std::atomic<bool>& stopping;
    const std::atomic<uint64_t>& shutdownDeadline;
    bool cleanup = false, authenticationRejected = false;
    bool transientPairing = false;
    uint64_t cleanupDeadline = 0;
};
thread_local Operation* operation = nullptr;
class OperationScope {
public:
    explicit OperationScope(Operation& current) : previous_(operation) { operation = &current; }
    ~OperationScope() { operation = previous_; }
private:
    Operation* previous_;
};
std::string TagUtf8(const std::wstring& value) {
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string data(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), data.data(), count, nullptr, nullptr);
    // Upstream DMAP builder has a 1024-byte buffer. Keep all three fields bounded.
    if (data.size() > 240) { size_t end = 240; while (end && (static_cast<unsigned char>(data[end]) & 0xc0) == 0x80) --end; data.resize(end); }
    return data;
}
}
extern "C" int airplay_raop_cancelled() {
    return operation && !operation->cleanup && operation->stopping.load() &&
           GetTickCount64() >= operation->shutdownDeadline.load();
}
extern "C" unsigned long long airplay_raop_deadline() {
    if (operation && operation->cleanup) {
        if (!operation->cleanupDeadline) operation->cleanupDeadline = GetTickCount64() + 300;
        return operation->cleanupDeadline;
    }
    if (operation && operation->stopping.load()) return operation->shutdownDeadline.load();
    return GetTickCount64() + 3000;
}
extern "C" void airplay_raop_event(const char* stage, int code) {
    if (operation && (code == 401 || code == 403 || code == 470))
        operation->authenticationRejected = true;
    if (!strcmp(stage, "timing_rx")) { ++protocol.timingReceived; return; }
    if (!strcmp(stage, "timing_tx")) { ++protocol.timingSent; return; }
    if (code == 0) { protocol.sentBytes = 0; protocol.receivedBytes = 0; }
    std::lock_guard<std::mutex> lock(protocol.mutex);
    if (code < 0 || code >= 400) {
        RememberFailure({GetTickCount64(), stage, "rtsp_response", code});
    }
    protocol.stage = stage; protocol.code = code;
    protocol.rtspHistory.push_back({GetTickCount64(), stage, code});
    if (protocol.rtspHistory.size() > 128) protocol.rtspHistory.pop_front();
}
extern "C" void airplay_raop_io_failure(const char* reason, int code) {
    std::lock_guard<std::mutex> lock(protocol.mutex);
    RememberFailure({GetTickCount64(), protocol.stage, reason, code});
}
extern "C" int airplay_raop_pairing(struct rtspcl_s* client) {
    if (!operation || !operation->transientPairing) return 1;
    try {
        airplay::pairing::TransientPair([&](const char* path, const airplay::pairing::Bytes& body) {
            airplay::pairing::Bytes response(4096);
            int size = rtspcl_pairing_post(client, path, body.data(), int(body.size()), response.data(), int(response.size()));
            if (size < 0) throw std::runtime_error("Transient pairing request failed");
            response.resize(size);
            return response;
        });
        return 1;
    } catch (const std::exception&) {
        airplay_raop_io_failure("transient_pairing_failed", 0);
        return 0;
    }
}
extern "C" void airplay_raop_io_bytes(int sent, int received) {
    protocol.sentBytes += sent;
    protocol.receivedBytes += received;
}
extern "C" void airplay_raop_payload_result(const char* type, int code) {
    if (!type) return;
    if (!strcmp(type, "application/x-dmap-tagged")) protocol.metadataReply = code;
    if (!strncmp(type, "image/", 6)) protocol.artworkReply = code;
}

namespace airplay {
struct RaopSession::Impl {
    SocketRuntime sockets;
    std::shared_ptr<CryptoRuntime> runtime;
    Operation operation;
    raopcl_s* client = nullptr;
    Impl(std::shared_ptr<CryptoRuntime> runtime, const std::atomic<bool>& stopping,
         const std::atomic<uint64_t>& shutdownDeadline)
        : runtime(std::move(runtime)), operation{stopping, shutdownDeadline} {}
};
RaopSession::RaopSession(std::shared_ptr<CryptoRuntime> runtime, const std::atomic<bool>& stopping,
                         const std::atomic<uint64_t>& shutdownDeadline)
    : impl_(std::make_unique<Impl>(std::move(runtime), stopping, shutdownDeadline)) {}
RaopSession::~RaopSession() { Close(); }
RaopSession::Connection RaopSession::Connect(const Receiver& target, const std::string& remoteId, const std::string& remoteToken) {
    Close();
    if (target.passwordRequired) return Connection::PasswordRequired;
    if (ReceiverNeedsPin(target) && target.pairingSecret.empty()) return Connection::AuthenticationRequired;
    impl_->operation.transientPairing = ReceiverUsesTransientPairing(target);
    OperationScope scope(impl_->operation);
    auto toUtf8 = [](const std::wstring& value) {
        int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
        std::string out(count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), count, nullptr, nullptr);
        return out;
    };
    auto hostName = toUtf8(target.host);
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostName.c_str(), nullptr, &hints, &addresses)) return Connection::ResolveFailed;
    auto address = reinterpret_cast<sockaddr_in*>(addresses->ai_addr)->sin_addr;
    freeaddrinfo(addresses);
    in_addr local{};
    // Bind RTP to the interface Windows routes to this receiver.
    SOCKET route = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr = address;
    destination.sin_port = htons(target.port);
    sockaddr_in source{}; int sourceSize = sizeof(source);
    if (route == INVALID_SOCKET || connect(route, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) ||
        getsockname(route, reinterpret_cast<sockaddr*>(&source), &sourceSize)) {
        if (route != INVALID_SOCKET) closesocket(route);
        return Connection::InterfaceFailed;
    }
    local = source.sin_addr;
    closesocket(route);
    auto cryptoType = target.encryption.find('1') != std::string::npos ? RAOP_RSA : RAOP_CLEAR;
    auto encryption = target.encryption.substr(0, 15);
    auto capabilities = target.metadata.substr(0, 15);
    impl_->client = raopcl_create(local, 0, 0, remoteId.empty() ? nullptr : const_cast<char*>(remoteId.c_str()), remoteToken.empty() ? nullptr : const_cast<char*>(remoteToken.c_str()), RAOP_ALAC, 352, 22050,
                           cryptoType, cryptoType == RAOP_RSA, target.pairingSecret.empty() ? nullptr : const_cast<char*>(target.pairingSecret.c_str()), nullptr,
                           const_cast<char*>(encryption.c_str()), const_cast<char*>(capabilities.c_str()),
                           target.model.c_str(), 44100, 16, 2, -6);
    impl_->operation.authenticationRejected = false;
    if (!impl_->client || !raopcl_connect(impl_->client, address, target.port, true)) {
        if (impl_->operation.authenticationRejected)
            return Connection::AuthenticationRequired;
        return Connection::Failed;
    }
    { std::lock_guard<std::mutex> lock(protocol.mutex); protocol.failureEpisodeEnded = true; }
    return Connection::Connected;
}
bool RaopSession::IsOpen() const noexcept { return impl_->client != nullptr; }
void RaopSession::Close() {
    if (!impl_->client) return;
    OperationScope scope(impl_->operation);
    impl_->operation.cleanup = impl_->operation.stopping.load();
    raopcl_destroy(impl_->client);
    impl_->client = nullptr;
    impl_->operation.cleanup = false;
    impl_->operation.cleanupDeadline = 0;
}
void RaopSession::Flush() {
    OperationScope scope(impl_->operation);
    raopcl_stop(impl_->client); raopcl_flush(impl_->client);
}
void RaopSession::Pause() {
    OperationScope scope(impl_->operation);
    raopcl_pause(impl_->client); raopcl_flush(impl_->client);
}
bool RaopSession::AcceptFrames() {
    OperationScope scope(impl_->operation);
    return raopcl_accept_frames(impl_->client);
}
bool RaopSession::Send(const int16_t* samples, int frames) {
    OperationScope scope(impl_->operation);
    uint64_t playtime;
    return raopcl_send_chunk(impl_->client, reinterpret_cast<uint8_t*>(const_cast<int16_t*>(samples)), frames, &playtime);
}
bool RaopSession::Progress(uint32_t elapsedMs, uint32_t durationMs) {
    OperationScope scope(impl_->operation);
    return raopcl_set_progress_ms(impl_->client, elapsedMs, durationMs);
}
RaopSession::MetadataResult RaopSession::Metadata(const TrackMetadata& track) {
    OperationScope scope(impl_->operation);
    auto title = TagUtf8(track.title), artist = TagUtf8(track.artist), album = TagUtf8(track.album);
    detail::MetadataTiming timing{};
    timing.title = track.title;
    timing.artist = track.artist;
    timing.album = track.album;
    timing.streamStart = track.streamStart;
    timing.detected = track.detected;
    timing.coverConsumed = track.coverConsumed;
    timing.artworkBytes = track.artwork.size();
    protocol.metadataReply = 0;
    timing.tagsStarted = GetTickCount64();
    bool text = raopcl_set_daap(impl_->client, 3, "minm", 's', title.data(), "asar", 's', artist.data(), "asal", 's', album.data()) && protocol.metadataReply.load() == 200;
    timing.tagsFinished = GetTickCount64();
    timing.tagsCode = protocol.metadataReply.load();
    protocol.artworkReply = 0;
    timing.artworkStarted = GetTickCount64();
    bool art = raopcl_set_artwork(impl_->client, const_cast<char*>(track.artwork.empty() ? "image/none" : "image/jpeg"),
                                int(track.artwork.size()), track.artwork.empty() ? const_cast<char*>("") : reinterpret_cast<char*>(const_cast<uint8_t*>(track.artwork.data())));
    art = art && protocol.artworkReply.load() == 200;
    timing.artworkFinished = GetTickCount64();
    timing.artworkCode = protocol.artworkReply.load();
    {
        std::lock_guard<std::mutex> lock(protocol.mutex);
        protocol.metadataHistory.push_back(timing);
        if (protocol.metadataHistory.size() > 32) protocol.metadataHistory.pop_front();
    }
    return {text, art};
}
bool RaopSession::KeepAlive() {
    OperationScope scope(impl_->operation);
    return raopcl_keepalive(impl_->client);
}
}
