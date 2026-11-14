#include "pairing_controller.h"
#include <algorithm>

namespace airplay::aimp {
PairingController::PairingController(HINSTANCE instance, ReceiverController& receivers, std::shared_ptr<Engine> engine, std::shared_ptr<CryptoRuntime> runtime)
    : instance_(instance), receivers_(receivers), engine_(std::move(engine)), runtime_(std::move(runtime)) {}
PairingController::~PairingController() {
    active_ = false;
    Cancel();
    if (pairingTask_.valid()) { try { pairingTask_.get(); } catch (...) {} }
}
void PairingController::Cancel() {
    ++pairingGeneration_;
    engine_->SetPairingStatus({});
    if (pairing_) pairing_->Cancel();
    if (pinDialog_) EndDialog(pinDialog_, IDCANCEL);
}
INT_PTR CALLBACK PairingController::PinDialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        auto* self = reinterpret_cast<PairingController*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp); self->pinDialog_ = window;
        SendDlgItemMessageW(window, 102, EM_SETLIMITTEXT, 4, 0);
        EnableWindow(GetDlgItem(window, IDOK), FALSE);
        return TRUE;
    }
    auto* self = reinterpret_cast<PairingController*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_COMMAND && self) {
        if (LOWORD(wp) == 102 && HIWORD(wp) == EN_CHANGE) {
            EnableWindow(GetDlgItem(window, IDOK), GetWindowTextLengthW(GetDlgItem(window, 102)) == 4); return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            wchar_t pin[5]{}; GetDlgItemTextW(window, 102, pin, 5);
            if (wcslen(pin) != 4 || !std::all_of(pin, pin + 4, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return TRUE;
            self->enteredPin_.assign(pin, pin + 4); EndDialog(window, IDOK); return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
    }
    return FALSE;
}
void PairingController::Poll() {
    if (pinDialog_) return;
    if (pairingTask_.valid()) {
        if (pairingTask_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        bool current = active_ && pairingAttempt_ == pairingGeneration_ && pairingId_ == receivers_.Config().selected;
        std::string result;
        try { result = pairingTask_.get(); }
        catch (const std::exception& error) {
            pairing_.reset();
            if (current) {
                auto text = std::string(error.what());
                int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
                std::wstring message(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), message.data(), n);
                receivers_.Select(L"");
                MessageBoxW(nullptr, message.c_str(), L"AirPlay", MB_OK | MB_ICONERROR);
            }
            return;
        }
        if (!current) { pairing_.reset(); return; }
        if (pairingStarting_) {
            enteredPin_.clear();
            engine_->SetPairingStatus(L"Введите код с экрана Apple TV");
            auto answer = DialogBoxParamW(instance_, MAKEINTRESOURCEW(101), GetActiveWindow(), PinDialog, reinterpret_cast<LPARAM>(this));
            pinDialog_ = nullptr;
            if (answer != IDOK || !active_ || pairingAttempt_ != pairingGeneration_) {
                pairing_->Cancel(); pairing_.reset();
                if (active_ && pairingAttempt_ == pairingGeneration_) receivers_.Select(L"");
                return;
            }
            pairingStarting_ = false;
            engine_->SetPairingStatus(L"Подтверждение кода Apple TV");
            auto session = pairing_; auto pin = enteredPin_; enteredPin_.clear();
            pairingTask_ = std::async(std::launch::async, [session, pin] { return session->Finish(pin); });
        } else {
            pairing_.reset();
            engine_->SetPairingStatus({});
            if (!receivers_.CompletePairing(pairingId_, result))
                MessageBoxW(nullptr, L"Код принят. Не удалось сохранить привязку в хранилище Windows. После перезапуска AIMP повторите ввод кода.", L"AirPlay", MB_OK | MB_ICONERROR);
        }
        return;
    }
    if (!engine_->NeedsPairing()) return;
    auto receiver = receivers_.Selected();
    if (!receiver) return;
    pairingId_ = receiver->id; pairingAttempt_ = pairingGeneration_; pairingStarting_ = true;
    pairing_ = std::make_shared<airplay::PinPairing>(*receiver, runtime_);
    engine_->Select(nullptr);
    engine_->SetPairingStatus(L"Запрос кода на Apple TV");
    auto session = pairing_;
    pairingTask_ = std::async(std::launch::async, [session] { session->Start(); return std::string(); });
}
}
