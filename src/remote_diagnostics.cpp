#include "remote_diagnostics.h"
#include <windows.h>
#include <utility>

namespace airplay {
void RemoteDiagnostics::Result(const std::string& command, long result) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.command = command;
    status_.result = result;
    Append({GetTickCount64(), command, {}, result});
}
void RemoteDiagnostics::Error(const std::string& stage, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.error = stage + ": " + detail;
    Append({GetTickCount64(), {}, status_.error});
}
void RemoteDiagnostics::Append(Event event) {
    status_.history.push_back(std::move(event));
    if (status_.history.size() > 64) status_.history.pop_front();
}
RemoteDiagnostics::Status RemoteDiagnostics::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}
}
