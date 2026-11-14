#include "receiver_controller.h"
#include "aimp_sdk_helpers.h"
#include "discovery.h"
#include "pairing_credentials.h"
#include <algorithm>

namespace airplay::aimp {
ReceiverController::ReceiverController(std::wstring path, std::shared_ptr<Engine> engine, RemoteControl& remote)
    : path_(std::move(path)), config_(LoadReceivers(path_)), engine_(std::move(engine)), remote_(remote) {
    for (auto& receiver : config_.receivers) receiver.pairingSecret = LoadPairingSecret(receiver.id);
    if (auto receiver = Selected()) { remote_.Select(receiver->host); engine_->Select(receiver); }
}
ReceiverController::~ReceiverController() {
    cancelled_ = true;
    if (discovery_.valid()) { try { discovery_.get(); } catch (...) {} }
}
const Receiver* ReceiverController::Selected() const {
    auto found = std::find_if(config_.receivers.begin(), config_.receivers.end(), [&](const auto& r) { return r.id == config_.selected; });
    return found == config_.receivers.end() ? nullptr : &*found;
}
void ReceiverController::Select(const std::wstring& id) {
    if (beforeSelection_) beforeSelection_();
    auto receiver = std::find_if(config_.receivers.begin(), config_.receivers.end(), [&](const auto& r) { return r.id == id; });
    if (receiver != config_.receivers.end() && !SaveReceiverCache(path_, *receiver))
        throw SdkError{HRESULT_FROM_WIN32(GetLastError())};
    if (id != config_.selected) {
        if (!SaveSelection(path_, id)) throw SdkError{HRESULT_FROM_WIN32(GetLastError())};
        config_.selected = id;
    }
    engine_->Select(receiver == config_.receivers.end() ? nullptr : &*receiver);
    remote_.Select(receiver == config_.receivers.end() ? L"" : receiver->host);
}
bool ReceiverController::CompletePairing(const std::wstring& id, const std::string& secret) {
    auto found = std::find_if(config_.receivers.begin(), config_.receivers.end(), [&](const auto& r) { return r.id == id; });
    if (found == config_.receivers.end()) return false;
    found->pairingSecret = secret;
    bool saved = SavePairingSecret(id, secret);
    engine_->Select(&*found);
    return saved;
}
void ReceiverController::Poll() {
    ApplyDiscovery();
    if (!lastDiscovery_ || GetTickCount64() - lastDiscovery_ >= 10000) StartDiscovery();
}
void ReceiverController::StartDiscovery() {
    if (discovery_.valid()) return;
    lastDiscovery_ = GetTickCount64();
    searchCaption_ = L"Поиск приёмников…";
    int seconds = config_.receivers.empty() ? 1 : 4;
    cancelled_ = false;
    auto* cancelled = &cancelled_;
    discovery_ = std::async(std::launch::async, [seconds, cancelled] { return DiscoverReceivers(seconds, cancelled); });
}
void ReceiverController::ApplyDiscovery() {
    if (!discovery_.valid() || discovery_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto found = discovery_.get();
    auto configured = airplay::LoadReceivers(path_).receivers;
    for (const auto& receiver : found) {
        auto cached = std::find_if(configured.begin(), configured.end(), [&](const auto& r) { return r.id == receiver.id; });
        if (cached == configured.end()) configured.push_back(receiver);
        else *cached = receiver;
    }
    if (engine_->RemoteOutputActive() && std::none_of(configured.begin(), configured.end(), [&](const auto& r) { return r.id == config_.selected; })) {
        if (auto active = Selected()) configured.push_back(*active);
    }
    auto cachedSelection = std::find_if(configured.begin(), configured.end(), [&](const auto& r) { return r.id == config_.selected; });
    for (auto& receiver : configured) {
        auto old = std::find_if(config_.receivers.begin(), config_.receivers.end(), [&](const auto& r) { return r.id == receiver.id; });
        receiver.pairingSecret = old != config_.receivers.end() ? old->pairingSecret : airplay::LoadPairingSecret(receiver.id);
    }
    if (cachedSelection != configured.end() && !airplay::SaveReceiverCache(path_, *cachedSelection))
        throw SdkError{HRESULT_FROM_WIN32(GetLastError())};
    config_.receivers = std::move(configured);
    auto selected = Selected();
    if (!selected) {
        config_.selected.clear(); engine_->Select(nullptr);
        remote_.Select(L"");
    } else {
        remote_.Select(selected->host);
        if (!restored_) engine_->Select(selected);
        else engine_->RefreshReceiver(*selected);
    }
    restored_ = true;
    ++revision_;
    searchCaption_ = found.empty() ? L"AirPlay-приёмники не найдены в локальной сети" : L"Найдено по сети: " + std::to_wstring(found.size());
}
}
