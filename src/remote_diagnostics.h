#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace airplay {
class RemoteDiagnostics {
public:
    struct Event {
        uint64_t tick;
        std::string command, error;
        long result = 0;
    };
    struct Status {
        std::string command, error;
        long result = 0;
        std::deque<Event> history;
    };
    void Result(const std::string& command, long result);
    void Error(const std::string& stage, const std::string& detail);
    Status Snapshot() const;
private:
    void Append(Event event);
    mutable std::mutex mutex_;
    Status status_;
};
}
