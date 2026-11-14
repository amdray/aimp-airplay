#pragma once
#include "engine.h"
#include "raop_session.h"
#include "remote_diagnostics.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace airplay {
struct Engine::Impl {
    struct Packet { std::vector<uint8_t> bytes; int frames, bits, channels, rate; };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Packet> packets;
    size_t queuedBytes = 0;
    Receiver receiver{};
    std::string remoteId, remoteToken;
    bool enabled = false;
    bool connecting = false;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> shutdownDeadline{0};
    std::unique_ptr<RaopSession> session;
    std::atomic<bool> remoteOutputActive{false};
    std::atomic<bool> recovering{false};
    std::atomic<bool> needsPairing{false};
    uint64_t revision = 0, flush = 0;
    int playbackState = 2;
    bool captureAvailable = true;
    uint64_t releaseDueTick = 0;
    TrackMetadata metadata;
    uint64_t metadataRevision = 0;
    uint32_t elapsedMs = 0, durationMs = 0;
    uint64_t progressSampledTick = 0, progressStreamStart = 0;
    std::atomic<int> metadataResult{0}, artworkResult{0};
    std::wstring status = L"Отключено";
    std::wstring pairingStatus;
    std::shared_ptr<const RemoteDiagnostics> remoteDiagnostics;
    std::thread worker;
    std::thread reporter;
    std::wstring diagnosticPath;
    std::atomic<uint64_t> callbacks{0}, acceptedFrames{0}, rejectedCallbacks{0}, sentChunks{0}, lastPcmTick{0};
    std::atomic<int> inputBits{0}, inputChannels{0}, inputRate{0};
    void ClearPackets() { packets.clear(); queuedBytes = 0; } // Caller holds mutex.
    void Status(const std::wstring& value) { std::lock_guard<std::mutex> lock(mutex); status = value; }
    void Report() noexcept;
    void ReportLoop() noexcept;
    void Run() noexcept;
};
}
