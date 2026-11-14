#pragma once
#include "receivers.h"
#include "track_metadata.h"
#include <atomic>
#include <memory>

namespace airplay {
class CryptoRuntime;
class RaopSession {
public:
    enum class Connection { Connected, ResolveFailed, InterfaceFailed, AuthenticationRequired,
                            Failed, PasswordRequired };
    struct MetadataResult { bool text, artwork; };
    RaopSession(std::shared_ptr<CryptoRuntime> runtime, const std::atomic<bool>& stopping,
                const std::atomic<uint64_t>& shutdownDeadline);
    ~RaopSession();
    Connection Connect(const Receiver& receiver, const std::string& remoteId, const std::string& remoteToken);
    bool IsOpen() const noexcept;
    void Close();
    void Flush();
    void Pause();
    bool AcceptFrames();
    bool Send(const int16_t* samples, int frames);
    bool Progress(uint32_t elapsedMs, uint32_t durationMs);
    MetadataResult Metadata(const TrackMetadata& track);
    bool KeepAlive();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
