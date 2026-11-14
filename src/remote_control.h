#pragma once
#include <string>
#include <functional>
#include <memory>
#include <cstdint>
namespace airplay {
enum class RemoteCommand { PlayPause, Play, Pause, Stop, Next, Previous, Restart };
class RemoteControl {
public:
    using Callback = std::function<void(RemoteCommand, uint32_t)>;
    using ErrorCallback = std::function<void(const std::string&, const std::string&)>;
    explicit RemoteControl(Callback callback, ErrorCallback error = {});
    ~RemoteControl();
    void Select(const std::wstring& receiverHost);
    bool IsCurrent(uint32_t generation) const;
    const std::string& Id() const;
    const std::string& Token() const;
    unsigned short Port() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
