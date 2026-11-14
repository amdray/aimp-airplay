#include "engine_impl.h"
#include "protocol_diagnostics.h"
#include "dsp_bridge.h"
#include <sstream>

namespace airplay::detail {
ProtocolDiagnostics protocol;
void RecordPlaybackEvent(const char* event, int state) noexcept {
    try {
        std::lock_guard<std::mutex> lock(protocol.mutex);
        protocol.playbackHistory.push_back({GetTickCount64(), event, state});
        if (protocol.playbackHistory.size() > 64) protocol.playbackHistory.pop_front();
    } catch (...) {}
}
void RememberFailure(const ProtocolFailure& failure) {
    auto measured = failure;
    measured.sentBytes = protocol.sentBytes.load();
    measured.receivedBytes = protocol.receivedBytes.load();
    if (!protocol.haveFirstFailure || protocol.failureEpisodeEnded) {
        protocol.firstFailure = measured;
        protocol.haveFirstFailure = true;
        protocol.failureEpisodeEnded = false;
    }
    protocol.failures.push_back(measured);
    if (protocol.failures.size() > 32) protocol.failures.pop_front();
}
}

namespace {
std::string JsonString(const std::wstring& value) {
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0'), out = "\"";
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), utf8.data(), size, nullptr, nullptr);
    for (unsigned char ch : utf8) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += char(ch); }
        else if (ch < 32) { const char* hex = "0123456789abcdef"; out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 15]; }
        else out += char(ch);
    }
    return out + '"';
}
void WriteFailure(std::ostream& output, const airplay::detail::ProtocolFailure& failure) {
    output << "{\"tick_ms\":" << failure.tick
           << ",\"method\":" << JsonString(std::wstring(failure.method.begin(), failure.method.end()))
           << ",\"reason\":" << JsonString(std::wstring(failure.reason.begin(), failure.reason.end()))
           << ",\"code\":" << failure.code
           << ",\"sent_bytes\":" << failure.sentBytes
           << ",\"received_bytes\":" << failure.receivedBytes << '}';
}
}

namespace airplay {
using detail::protocol;
void Engine::Impl::Report() noexcept {
    try {
        std::ostringstream output;
        auto tick = GetTickCount64();
        std::wstring statusSnapshot, nameSnapshot, hostSnapshot, titleSnapshot, artistSnapshot, albumSnapshot;
        bool selectedSnapshot, captureSnapshot;
        int playbackSnapshot;
        auto remote = remoteDiagnostics ? remoteDiagnostics->Snapshot() : RemoteDiagnostics::Status{};
        size_t queueSnapshot, artworkSnapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            statusSnapshot = pairingStatus.empty() ? status : pairingStatus;
            nameSnapshot = receiver.name;
            hostSnapshot = receiver.host;
            titleSnapshot = metadata.title;
            artistSnapshot = metadata.artist;
            albumSnapshot = metadata.album;
            selectedSnapshot = enabled;
            captureSnapshot = captureAvailable;
            playbackSnapshot = playbackState;
            queueSnapshot = queuedBytes;
            artworkSnapshot = metadata.artwork.size();
        }
        unsigned long dsp[2]{};
        HMODULE dspModule = nullptr;
        bool dspLoaded = GetModuleHandleExW(0, L"dsp_airplay.dll", &dspModule);
        if (dspLoaded) {
            using ReadDsp = void (WINAPI*)(unsigned long*);
            auto read = reinterpret_cast<ReadDsp>(GetProcAddress(dspModule, "AirPlayDSPStatus"));
            if (read) read(dsp);
            FreeLibrary(dspModule);
        }
        {
            output << "{\n  \"version\": \"" AIRPLAY_VERSION "\",\n  \"pid\": " << GetCurrentProcessId()
                   << ",\n  \"updated_tick_ms\": " << tick << ",\n  \"status\": " << JsonString(statusSnapshot)
                   << ",\n  \"selected\": " << (selectedSnapshot ? "true" : "false")
                   << ",\n  \"receiver\": " << JsonString(nameSnapshot)
                   << ",\n  \"host\": " << JsonString(hostSnapshot)
                   << ",\n  \"queued_bytes\": " << queueSnapshot
                   << ",\n  \"capture_available\": " << (captureSnapshot ? "true" : "false")
                   << ",\n  \"remote_output_active\": " << (remoteOutputActive.load() ? "true" : "false");
            output << ",\n  \"playback_state\": " << playbackSnapshot << ",\n  \"title\": " << JsonString(titleSnapshot)
                   << ",\n  \"artist\": " << JsonString(artistSnapshot) << ",\n  \"album\": " << JsonString(albumSnapshot)
                   << ",\n  \"artwork_bytes\": " << artworkSnapshot;
        }
        {
            std::lock_guard<std::mutex> lock(protocol.mutex);
            std::wstring stage(protocol.stage.begin(), protocol.stage.end());
            output << ",\n  \"rtsp_method\": " << JsonString(stage) << ",\n  \"rtsp_code\": " << protocol.code;
            output << ",\n  \"connection_failures\": [";
            bool first = true;
            for (const auto& failure : protocol.failures) {
                if (!first) output << ',';
                first = false;
                WriteFailure(output, failure);
            }
            output << ']';
            output << ",\n  \"progress_history\": [";
            first = true;
            for (const auto& update : protocol.progressHistory) {
                if (!first) output << ',';
                first = false;
                output << "{\"stream_start_ms\":" << update.streamStart
                       << ",\"sampled_ms\":" << update.sampled
                       << ",\"started_ms\":" << update.started
                       << ",\"finished_ms\":" << update.finished
                       << ",\"elapsed_ms\":" << update.elapsed
                       << ",\"duration_ms\":" << update.duration
                       << ",\"succeeded\":" << (update.succeeded ? "true" : "false") << '}';
            }
            output << "],\n  \"rtsp_history\": [";
            first = true;
            for (const auto& event : protocol.rtspHistory) {
                if (!first) output << ',';
                first = false;
                output << "{\"tick_ms\":" << event.tick
                       << ",\"method\":" << JsonString(std::wstring(event.method.begin(), event.method.end()))
                       << ",\"code\":" << event.code << '}';
            }
            output << ']';
            output << ",\n  \"playback_history\": [";
            first = true;
            for (const auto& event : protocol.playbackHistory) {
                if (!first) output << ',';
                first = false;
                output << "{\"tick_ms\":" << event.tick
                       << ",\"event\":" << JsonString(std::wstring(event.method.begin(), event.method.end()))
                       << ",\"state\":" << event.code << '}';
            }
            output << ']';
            output << ",\n  \"first_connection_failure\": ";
            if (!protocol.haveFirstFailure) output << "null";
            else WriteFailure(output, protocol.firstFailure);
            output << ",\n  \"metadata_history\": [";
            first = true;
            for (const auto& timing : protocol.metadataHistory) {
                if (!first) output << ',';
                first = false;
                output << "{\"stream_start_ms\":" << timing.streamStart
                       << ",\"title\":" << JsonString(timing.title)
                       << ",\"artist\":" << JsonString(timing.artist)
                       << ",\"album\":" << JsonString(timing.album)
                       << ",\"detected_ms\":" << timing.detected
                       << ",\"cover_consumed_ms\":" << timing.coverConsumed
                       << ",\"tags_started_ms\":" << timing.tagsStarted
                       << ",\"tags_finished_ms\":" << timing.tagsFinished
                       << ",\"tags_code\":" << timing.tagsCode
                       << ",\"artwork_started_ms\":" << timing.artworkStarted
                       << ",\"artwork_finished_ms\":" << timing.artworkFinished
                       << ",\"artwork_code\":" << timing.artworkCode
                       << ",\"artwork_bytes\":" << timing.artworkBytes << '}';
            }
            output << ']';
        }
        output << ",\n  \"last_remote_command\": " << JsonString(std::wstring(remote.command.begin(), remote.command.end()))
               << ",\n  \"last_remote_result\": " << remote.result
               << ",\n  \"last_remote_error\": " << JsonString(std::wstring(remote.error.begin(), remote.error.end()));
        output << ",\n  \"remote_history\": [";
        bool firstRemoteEvent = true;
        for (const auto& event : remote.history) {
            if (!firstRemoteEvent) output << ',';
            firstRemoteEvent = false;
            output << "{\"tick_ms\":" << event.tick;
            if (event.command.empty()) {
                output << ",\"error\":" << JsonString(std::wstring(event.error.begin(), event.error.end()));
            } else {
                output << ",\"command\":" << JsonString(std::wstring(event.command.begin(), event.command.end()))
                       << ",\"result\":" << event.result;
            }
            output << '}';
        }
        output << ']';
        output << ",\n  \"dsp_loaded\": " << (dspLoaded ? "true" : "false")
               << ",\n  \"dsp_bind_error\": " << dsp[0]
               << ",\n  \"dsp_bound\": " << dsp[1]
               << ",\n  \"dsp_state\": " << DspState() << ",\n  \"pcm_callbacks\": " << callbacks.load()
               << ",\n  \"pcm_accepted_frames\": " << acceptedFrames.load() << ",\n  \"pcm_rejected_callbacks\": " << rejectedCallbacks.load()
               << ",\n  \"input_bits\": " << inputBits.load() << ",\n  \"input_channels\": " << inputChannels.load()
               << ",\n  \"input_rate\": " << inputRate.load() << ",\n  \"last_pcm_tick_ms\": " << lastPcmTick.load()
               << ",\n  \"alac_chunks_sent\": " << sentChunks.load() << ",\n  \"timing_received\": " << protocol.timingReceived.load()
               << ",\n  \"timing_sent\": " << protocol.timingSent.load()
               << ",\n  \"metadata_result\": " << metadataResult.load()
               << ",\n  \"artwork_result\": " << artworkResult.load()
               << ",\n  \"metadata_rtsp_code\": " << protocol.metadataReply.load() << ",\n  \"artwork_rtsp_code\": " << protocol.artworkReply.load() << "\n}\n";
        auto data = output.str();
        auto temp = diagnosticPath + L".tmp";
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        bool ok = WriteFile(file, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size();
        CloseHandle(file);
        if (ok) MoveFileExW(temp.c_str(), diagnosticPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    } catch (...) {}
}
void Engine::Impl::ReportLoop() noexcept {
    while (true) {
        Report();
        for (int i = 0; i < 10; ++i) {
            { std::lock_guard<std::mutex> lock(mutex); if (stopping) { return; } }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
}
