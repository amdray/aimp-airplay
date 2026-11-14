#include "engine_impl.h"
#include "pcm.h"
#include "protocol_diagnostics.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace airplay {
using detail::protocol;
void Engine::Impl::Run() noexcept {
    auto& raop = *session;
    PcmConverter converter;
    std::deque<int16_t> pcm;
    uint64_t version = 0, flushed = 0;
    bool paused = false;
    auto lastInput = std::chrono::steady_clock::now();
    auto lastKeepalive = lastInput;
    auto lastProgress = lastInput - std::chrono::seconds(5);
    uint64_t lastProgressStream = 0;
    uint64_t metadataVersion = UINT64_MAX;
    int previousPlayback = 2;
    uint64_t retryDue = 0;
    bool firstAudioPending = true;
    unsigned retryDelay = 1000;
    auto retry = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || revision != version || !enabled || playbackState != 2 || !captureAvailable) return;
        recovering = true;
        retryDue = GetTickCount64() + retryDelay;
        retryDelay = retryDelay < 5000 ? retryDelay * 2 : 10000;
        ClearPackets();
    };
    auto close = [&] {
        remoteOutputActive = false;
        raop.Close();
        converter.Reset(); pcm.clear(); paused = false;
        metadataVersion = UINT64_MAX;
        lastProgressStream = 0;
        lastProgress = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    };
    try {
        while (true) {
            Packet packet{};
            Receiver target;
            bool reconnect = false, selected = false, capture = false;
            uint64_t wantedFlush;
            int playback;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (pcm.size() < 704) {
                    auto ready = [&] {
                        return stopping || revision != version || (raop.IsOpen() && !packets.empty()) || flush != flushed;
                    };
                    if (!raop.IsOpen() && !retryDue && !releaseDueTick)
                        wake.wait(lock, ready);
                    else
                        wake.wait_for(lock, std::chrono::milliseconds(10), ready);
                }
                if (stopping) break;
                if (releaseDueTick && GetTickCount64() >= releaseDueTick) {
                    releaseDueTick = 0;
                    if (playbackState == 0 || !captureAvailable) {
                        ++revision; remoteOutputActive = false;
                        ClearPackets();
                    }
                }
                if (revision != version) {
                    version = revision; target = receiver; selected = enabled; reconnect = true;
                    retryDue = 0; retryDelay = 1000;
                    if (!enabled || playbackState != 2 || !captureAvailable) recovering = false;
                } else if (!raop.IsOpen() && retryDue && GetTickCount64() >= retryDue && enabled && captureAvailable && playbackState == 2) {
                    target = receiver; selected = true; reconnect = true;
                    retryDue = 0;
                    ClearPackets();
                }
                wantedFlush = flush;
                playback = playbackState;
                capture = captureAvailable;
                // Brief AIMP Stop/DSP restarts at track boundaries must not
                // tear down an established RAOP stream or expose local audio.
                if (releaseDueTick && remoteOutputActive.load()) {
                    if (playback == 0) playback = 2;
                    capture = true;
                }
                if (!reconnect && raop.IsOpen() && !packets.empty()) {
                    packet = std::move(packets.front());
                    queuedBytes -= packet.bytes.size();
                    packets.pop_front();
                }
            }
            if (reconnect) {
                close();
                if (!selected) { Status(L"Отключено"); continue; }
                if (playback == 0) { Status(L"Idle — трек остановлен"); continue; }
                if (!capture) { Status(L"Ожидание запуска DSP"); continue; }
                if (playback == 1) { Status(L"Пауза — AirPlay-сессия не открыта"); continue; }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    status = L"Подключение: " + target.name;
                    connecting = true;
                }
                struct ConnectionAttempt {
                    Impl& engine;
                    ~ConnectionAttempt() {
                        std::lock_guard<std::mutex> lock(engine.mutex);
                        engine.connecting = false;
                    }
                } attempt{*this};
                auto connection = raop.Connect(target, remoteId, remoteToken);
                if (connection == RaopSession::Connection::AuthenticationRequired) {
                    close();
                    std::lock_guard<std::mutex> lock(mutex);
                    if (revision == version && enabled && !stopping) {
                        needsPairing = true; recovering = false; retryDue = 0;
                        status = L"Требуется код с экрана Apple TV";
                    }
                    continue;
                }
                if (connection == RaopSession::Connection::PasswordRequired) { Status(L"Приёмник требует пароль — пока не поддерживается"); continue; }
                if (connection != RaopSession::Connection::Connected) {
                    retry(); close();
                    if (connection == RaopSession::Connection::ResolveFailed) Status(L"Не удалось определить адрес приёмника");
                    else if (connection == RaopSession::Connection::InterfaceFailed) Status(L"Не удалось выбрать сетевой интерфейс");
                    else Status(L"RAOP: подключение не удалось (протокол, пароль или сеть)");
                    continue;
                }
                lastInput = std::chrono::steady_clock::now();
                firstAudioPending = true;
                lastKeepalive = lastInput;
                retryDue = 0; retryDelay = 1000;
                bool keepClient;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    keepClient = !stopping && enabled && captureAvailable && playbackState != 0 && revision == version;
                    remoteOutputActive = keepClient;
                    recovering = false;
                    if (!keepClient || playbackState != 2) ClearPackets();
                    playback = playbackState;
                }
                packet.bytes.clear();
                if (!keepClient) { close(); continue; }
                Status(L"Подключено: " + target.name + L" — ожидание PCM из DSP");
            }
            if (!raop.IsOpen()) {
                if (wantedFlush != flushed) {
                    pcm.clear(); converter.Reset(); flushed = wantedFlush;
                }
                continue;
            }
            if (playback == 0) { close(); Status(L"Idle — трек остановлен"); continue; }
            if (wantedFlush != flushed) {
                raop.Flush();
                firstAudioPending = true;
                pcm.clear(); converter.Reset(); flushed = wantedFlush;
                packet.bytes.clear();
                lastProgress = std::chrono::steady_clock::now() - std::chrono::seconds(5);
            }
            if (playback == 1) {
                if (!paused) { raop.Pause(); pcm.clear(); converter.Reset(); paused = true; }
                Status(L"Подключено — пауза");
                packet.bytes.clear();
            } else if (previousPlayback == 1) paused = false;
            previousPlayback = playback;
            TrackMetadata track;
            bool sendMetadata = false;
            uint32_t position, duration;
            uint64_t progressSampled, progressStream, metadataStream;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (metadataVersion != metadataRevision && (!firstAudioPending || playback == 1)) { track = metadata; metadataVersion = metadataRevision; sendMetadata = true; }
                position = elapsedMs; duration = durationMs;
                progressSampled = progressSampledTick;
                progressStream = progressStreamStart;
                metadataStream = metadata.streamStart;
            }
            if (!packet.bytes.empty()) {
                auto converted = converter.Convert(packet.bytes.data(), packet.frames, packet.bits, packet.channels, packet.rate);
                pcm.insert(pcm.end(), converted.begin(), converted.end());
                lastInput = std::chrono::steady_clock::now();
                if (paused) { paused = false; Status(L"Передача звука"); }
            }
            bool sentAudio = false;
            if (pcm.size() >= 704 && raop.AcceptFrames()) {
                int16_t chunk[704];
                for (auto& sample : chunk) { sample = pcm.front(); pcm.pop_front(); }
                if (!raop.Send(chunk, 352)) {
                    retry(); close(); Status(L"RAOP: ошибка передачи"); continue;
                }
                Status(L"Передача звука");
                firstAudioPending = false;
                ++sentChunks;
                sentAudio = true;
            } else std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (sendMetadata) {
                auto result = raop.Metadata(track);
                metadataResult = result.text ? 1 : -1;
                artworkResult = result.artwork ? 1 : -1;
            }
            const bool newTrackProgress = progressStream != lastProgressStream;
            const bool progressDue = std::chrono::steady_clock::now() - lastProgress >= std::chrono::seconds(5);
            if (playback == 2 && progressStream == metadataStream && !firstAudioPending &&
                ((newTrackProgress && (sentAudio || sendMetadata)) || (sentAudio && progressDue))) {
                auto started = GetTickCount64();
                bool succeeded = raop.Progress(position, duration);
                auto finished = GetTickCount64();
                {
                    std::lock_guard<std::mutex> lock(protocol.mutex);
                    protocol.progressHistory.push_back({progressStream, progressSampled, started, finished, position, duration, succeeded});
                    if (protocol.progressHistory.size() > 64) protocol.progressHistory.pop_front();
                }
                lastProgress = std::chrono::steady_clock::now();
                lastProgressStream = progressStream;
            }
            if (!paused && std::chrono::steady_clock::now() - lastInput > std::chrono::milliseconds(750) && pcm.size() < 704) {
                raop.Pause(); paused = true;
                Status(L"Подключено — пауза / ожидание PCM");
            }
            // Cap latency if a receiver cannot keep pace: discard backlog and reset its RTP timeline.
            if (pcm.size() > 44100 * 4) { pcm.clear(); converter.Reset(); raop.Flush(); }
            if (std::chrono::steady_clock::now() - lastKeepalive > std::chrono::seconds(10)) {
                if (!raop.KeepAlive()) { retry(); close(); Status(L"RAOP: приёмник отключился — повторное подключение"); }
                lastKeepalive = std::chrono::steady_clock::now();
            }
        }
    } catch (...) { Status(L"Ошибка аудиотракта AirPlay"); }
    close();
}
Engine::Engine(std::shared_ptr<CryptoRuntime> runtime, const std::wstring& diagnosticPath,
               std::shared_ptr<const RemoteDiagnostics> remoteDiagnostics)
    : impl_(std::make_unique<Impl>()) {
    if (!runtime) throw std::invalid_argument("Engine requires CryptoRuntime");
    impl_->session = std::make_unique<RaopSession>(std::move(runtime), impl_->stopping, impl_->shutdownDeadline);
    impl_->remoteDiagnostics = std::move(remoteDiagnostics);
    impl_->diagnosticPath = diagnosticPath;
    try {
        impl_->worker = std::thread([this] { impl_->Run(); });
        if (!diagnosticPath.empty()) impl_->reporter = std::thread([this] { impl_->ReportLoop(); });
    } catch (...) {
        Shutdown();
        throw;
    }
}
Engine::~Engine() { Shutdown(); }
void Engine::Shutdown() {
    impl_->remoteOutputActive = false;
    impl_->recovering = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->stopping) {
            // Let an already running transaction finish briefly, avoiding an
            // unnecessary TCP abort when the receiver is still responsive.
            impl_->shutdownDeadline = GetTickCount64() + 300;
            impl_->stopping = true;
        }
    }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->reporter.joinable()) impl_->reporter.join();
    if (!impl_->diagnosticPath.empty()) { impl_->Status(L"Плагин остановлен"); impl_->Report(); }
}
void Engine::SetRemoteIdentity(std::string id, std::string token) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->enabled || id.size() != 16 || token.empty() || token.size() > 10 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c); }) ||
        !std::all_of(token.begin(), token.end(), [](char c) { return c >= '0' && c <= '9'; })) throw std::invalid_argument("Invalid DACP identity");
    impl_->remoteId = std::move(id); impl_->remoteToken = std::move(token);
}
void Engine::Select(const Receiver* receiver) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!receiver && !impl_->enabled) return;
    if (receiver && impl_->enabled && receiver->pairingSecret == impl_->receiver.pairingSecret && receiver->id == impl_->receiver.id && receiver->host == impl_->receiver.host && receiver->port == impl_->receiver.port &&
        (impl_->remoteOutputActive.load() || impl_->connecting || !impl_->captureAvailable || impl_->playbackState != 2)) return;
    impl_->remoteOutputActive = false;
    impl_->recovering = false;
    impl_->enabled = receiver != nullptr;
    impl_->needsPairing = false;
    if (receiver) impl_->receiver = *receiver;
    ++impl_->revision;
    impl_->ClearPackets();
    impl_->wake.notify_all();
}
void Engine::RefreshReceiver(const Receiver& receiver) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->enabled || impl_->receiver.id != receiver.id) return;
    bool changed = impl_->receiver.host != receiver.host || impl_->receiver.port != receiver.port ||
                   impl_->receiver.encryption != receiver.encryption || impl_->receiver.model != receiver.model ||
                   impl_->receiver.passwordRequired != receiver.passwordRequired || impl_->receiver.metadata != receiver.metadata ||
                   impl_->receiver.features != receiver.features || impl_->receiver.pairingFlags != receiver.pairingFlags;
    impl_->receiver = receiver;
    // Retain an existing stream; the next connection uses the refreshed endpoint.
    if (changed && !impl_->remoteOutputActive.load()) { ++impl_->revision; impl_->wake.notify_one(); }
}
void Engine::Push(const void* samples, int frames, int bits, int channels, int rate) noexcept {
    try {
        ++impl_->callbacks;
        impl_->inputBits = bits; impl_->inputChannels = channels; impl_->inputRate = rate;
        impl_->lastPcmTick = GetTickCount64();
        if (!samples || frames <= 0 || frames > 262144 || (bits != 16 && bits != 24 && bits != 32) ||
            (channels != 1 && channels != 2) || rate < 8000 || rate > 384000) { ++impl_->rejectedCallbacks; return; }
        size_t size = static_cast<size_t>(frames) * channels * (bits / 8);
        Impl::Packet packet{{}, frames, bits, channels, rate};
        packet.bytes.resize(size); memcpy(packet.bytes.data(), samples, size);
        std::lock_guard<std::mutex> lock(impl_->mutex);
        bool transition = impl_->releaseDueTick && impl_->remoteOutputActive.load();
        if (!impl_->enabled || impl_->stopping || impl_->playbackState != 2 ||
            (!impl_->captureAvailable && !transition)) { ++impl_->rejectedCallbacks; return; }
        if (impl_->queuedBytes + size > 2 * 1024 * 1024) {
            impl_->ClearPackets(); ++impl_->flush;
        }
        impl_->packets.push_back(std::move(packet)); impl_->queuedBytes += size;
        impl_->acceptedFrames += frames;
        impl_->wake.notify_one();
    } catch (...) {}
}
void Engine::Flush() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ClearPackets(); ++impl_->flush;
    impl_->wake.notify_one();
}
void Engine::SetPlaybackState(int state) {
    if (state < 0 || state > 2) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->playbackState == state) return;
    detail::RecordPlaybackEvent("state", state);
    impl_->playbackState = state;
    if (state != 2) impl_->recovering = false;
    if (state == 0 && impl_->remoteOutputActive.load()) {
        impl_->releaseDueTick = GetTickCount64() + 750;
    } else if (state == 0 || (state == 2 && !impl_->remoteOutputActive.load())) {
        ++impl_->revision;
        impl_->ClearPackets();
    }
    if (state != 0 && impl_->captureAvailable) impl_->releaseDueTick = 0;
    if (state == 1) { impl_->ClearPackets(); }
    impl_->wake.notify_one();
}
void Engine::SetCaptureAvailable(bool available) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->captureAvailable == available) return;
    impl_->captureAvailable = available;
    if (!available && impl_->remoteOutputActive.load()) impl_->releaseDueTick = GetTickCount64() + 750;
    else if (!impl_->remoteOutputActive.load()) { ++impl_->revision; impl_->ClearPackets(); }
    if (available && impl_->playbackState != 0) impl_->releaseDueTick = 0;
    impl_->wake.notify_one();
}
bool Engine::CaptureAvailable() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->captureAvailable;
}
void Engine::SetMetadata(TrackMetadata metadata) {
    if (metadata.artwork.size() > 2 * 1024 * 1024) metadata.artwork.clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->metadata = std::move(metadata); ++impl_->metadataRevision;
    impl_->wake.notify_one();
}
void Engine::SetProgress(uint32_t elapsedMs, uint32_t durationMs) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->elapsedMs = elapsedMs; impl_->durationMs = durationMs;
    impl_->progressSampledTick = GetTickCount64();
    impl_->progressStreamStart = protocol.streamStart.load();
}
std::wstring Engine::Status() { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->pairingStatus.empty() ? impl_->status : impl_->pairingStatus; }
void Engine::SetPairingStatus(std::wstring status) { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->pairingStatus = std::move(status); }
bool Engine::RemoteOutputActive() const noexcept { return impl_->remoteOutputActive.load(); }
bool Engine::MuteLocalOutput() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->remoteOutputActive.load() || impl_->recovering.load() ||
           (impl_->enabled && impl_->captureAvailable && !impl_->stopping &&
            !impl_->needsPairing && !impl_->receiver.passwordRequired);
}
bool Engine::NeedsPairing() const noexcept { return impl_->needsPairing.load(); }
}
