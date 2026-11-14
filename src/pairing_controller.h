#pragma once
#include "receiver_controller.h"
#include "pairing.h"
#include <windows.h>

namespace airplay::aimp {
class PairingController {
public:
    PairingController(HINSTANCE instance, ReceiverController& receivers,
                      std::shared_ptr<Engine> engine, std::shared_ptr<CryptoRuntime> runtime);
    ~PairingController();
    void Poll();
    void Cancel();
private:
    static INT_PTR CALLBACK PinDialog(HWND window, UINT message, WPARAM wp, LPARAM lp);
    HINSTANCE instance_;
    ReceiverController& receivers_;
    std::shared_ptr<Engine> engine_;
    std::shared_ptr<CryptoRuntime> runtime_;
    bool active_ = true;
    std::shared_ptr<PinPairing> pairing_;
    std::future<std::string> pairingTask_;
    std::wstring pairingId_;
    std::string enteredPin_;
    uint64_t pairingGeneration_ = 0, pairingAttempt_ = 0;
    bool pairingStarting_ = false;
    HWND pinDialog_ = nullptr;
};
}
