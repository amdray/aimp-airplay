#pragma once
#include "receivers.h"
#include "track_metadata.h"
#include <memory>
#include <string>
#include <cstdint>
namespace airplay {
class CryptoRuntime;
class RemoteDiagnostics;
class Engine {
public:
    explicit Engine(std::shared_ptr<CryptoRuntime> runtime, const std::wstring& diagnosticPath = {},
                    std::shared_ptr<const RemoteDiagnostics> remoteDiagnostics = {});
    ~Engine();
    void Select(const Receiver* receiver);
    void SetRemoteIdentity(std::string id, std::string token);
    void RefreshReceiver(const Receiver& receiver); // Update discovery data without interrupting an active session.
    void Push(const void* samples, int frames, int bits, int channels, int rate) noexcept;
    void Flush();
    void SetPlaybackState(int state); // AIMP: 0 stopped, 1 paused, 2 playing
    void SetCaptureAvailable(bool available);
    bool CaptureAvailable() const;
    void SetMetadata(TrackMetadata metadata);
    void SetProgress(uint32_t elapsedMs, uint32_t durationMs);
    std::wstring Status();
    bool RemoteOutputActive() const noexcept;
    bool MuteLocalOutput() const noexcept;
    bool NeedsPairing() const noexcept;
    void SetPairingStatus(std::wstring status);
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
