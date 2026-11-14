#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace airplay::detail {
struct ProtocolFailure {
    uint64_t tick;
    std::string method, reason;
    int code;
    uint64_t sentBytes = 0, receivedBytes = 0;
};
struct MetadataTiming {
    std::wstring title, artist, album;
    uint64_t streamStart, detected, coverConsumed;
    uint64_t tagsStarted, tagsFinished, artworkStarted, artworkFinished;
    size_t artworkBytes;
    int tagsCode, artworkCode;
};
struct ProgressUpdate {
    uint64_t streamStart, sampled, started, finished;
    uint32_t elapsed, duration;
    bool succeeded;
};
struct RtspEvent {
    uint64_t tick;
    std::string method;
    int code;
};
struct ProtocolDiagnostics {
    std::mutex mutex;
    std::string stage = "idle";
    int code = 0;
    std::deque<ProtocolFailure> failures;
    std::deque<MetadataTiming> metadataHistory;
    std::deque<ProgressUpdate> progressHistory;
    std::deque<RtspEvent> rtspHistory;
    std::deque<RtspEvent> playbackHistory;
    std::atomic<uint64_t> streamStart{0};
    ProtocolFailure firstFailure{};
    bool haveFirstFailure = false;
    bool failureEpisodeEnded = false;
    std::atomic<uint64_t> timingReceived{0}, timingSent{0};
    std::atomic<int> metadataReply{0}, artworkReply{0};
    std::atomic<uint64_t> sentBytes{0}, receivedBytes{0};
};
extern ProtocolDiagnostics protocol;
void RememberFailure(const ProtocolFailure& failure);
void RecordPlaybackEvent(const char* event, int state) noexcept;
}
