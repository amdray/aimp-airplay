#pragma once
#include "engine.h"
#include "remote_control.h"
#include <atomic>
#include <functional>
#include <future>

namespace airplay::aimp {
class ReceiverController {
public:
    ReceiverController(std::wstring path, std::shared_ptr<Engine> engine,
                       RemoteControl& remote);
    ~ReceiverController();
    void Poll();
    void CancelDiscovery() noexcept { cancelled_ = true; }
    void Select(const std::wstring& id);
    void BeforeSelection(std::function<void()> callback) { beforeSelection_ = std::move(callback); }
    const ReceiverConfig& Config() const { return config_; }
    const Receiver* Selected() const;
    const std::wstring& SearchCaption() const { return searchCaption_; }
    uint64_t Revision() const { return revision_; }
    bool CompletePairing(const std::wstring& id, const std::string& secret);
private:
    void StartDiscovery();
    void ApplyDiscovery();
    std::wstring path_;
    ReceiverConfig config_;
    std::shared_ptr<Engine> engine_;
    RemoteControl& remote_;
    std::function<void()> beforeSelection_;
    std::wstring searchCaption_ = L"Поиск приёмников…";
    uint64_t revision_ = 0, lastDiscovery_ = 0;
    bool restored_ = false;
    std::atomic_bool cancelled_{false};
    std::future<std::vector<Receiver>> discovery_;
};
}
