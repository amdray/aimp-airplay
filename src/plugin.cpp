#include <winsock2.h>
#include "apiPlugin.h"
#include "aimp_sdk_helpers.h"
#include "airplay_menu.h"
#include "pairing_controller.h"
#include "remote_commands.h"
#include "player_events.h"
#include "crypto_runtime.h"
#include "dsp_bridge.h"
#include <atomic>
#include <new>

namespace {
using namespace airplay::aimp;
class Plugin final : public IAIMPPlugin {
public:
    HRESULT WINAPI QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        // IAIMPPlugin has no IID in the SDK; the host receives it from the export.
        if (IsEqualIID(iid, IID_IUnknown)) {
            *out = static_cast<IAIMPPlugin*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return ++refs_; }
    ULONG WINAPI Release() override { auto n = --refs_; if (!n) delete this; return n; }
    TChar* WINAPI InfoGet(int index) override {
        switch (index) {
        case AIMP_PLUGIN_INFO_NAME: return const_cast<TChar*>(L"AirPlay Sender");
        case AIMP_PLUGIN_INFO_AUTHOR: return const_cast<TChar*>(L"aimp-airplay contributors");
        case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION: return const_cast<TChar*>(L"Поиск AirPlay-приёмников и передача звука через DSP");
        case AIMP_PLUGIN_INFO_FULL_DESCRIPTION: return const_cast<TChar*>(L"v" AIRPLAY_VERSION L": передача звука, тегов и обложки. Привязка Apple TV через код с экрана. Управление с пульта Apple TV.");
        default: return nullptr;
        }
    }
    LongWord WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }
    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        if (!core) return E_POINTER;
        if (core_) return E_UNEXPECTED;
        HRESULT result = E_FAIL;
        try {
            core_ = ComPtr<IAIMPCore>(core);
            ComPtr<IAIMPString> profile;
            Check(core->GetPath(AIMP_CORE_PATH_PROFILE, profile.put()));
            std::wstring profilePath(profile->GetData(), profile->GetLength());
            if (profilePath.empty()) throw SdkError{E_FAIL};
            if (profilePath.back() != L'\\' && profilePath.back() != L'/') profilePath += L'\\';
            HMODULE module = nullptr;
            Check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                  reinterpret_cast<LPCWSTR>(&WindowProc), &module) ? S_OK : E_FAIL);
            wchar_t filename[32768]{};
            auto length = GetModuleFileNameW(module, filename, 32768);
            if (!length || length == 32768) throw SdkError{E_FAIL};
            std::wstring directory(filename, length);
            directory.erase(directory.find_last_of(L"\\/"));
            runtime_ = std::make_shared<airplay::CryptoRuntime>(directory);
            remoteDiagnostics_ = std::make_shared<airplay::RemoteDiagnostics>();
            engine_ = std::make_shared<airplay::Engine>(runtime_, profilePath + L"AirPlay-status.json", remoteDiagnostics_);
            airplay::SetEngine(engine_);
            Check(core->QueryInterface(IID_IAIMPServicePlayer, player_.out()));
            core->QueryInterface(IID_IAIMPServiceAlbumArt, albumArt_.out());
            metadata_ = std::make_unique<PlayerMetadata>(core, player_.get(), albumArt_.get(), engine_);
            metadata_->Update();
            Check(core->QueryInterface(IID_IAIMPServiceMessageDispatcher, dispatcher_.out()));
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = module;
            windowClass.lpszClassName = L"AimpAirPlaySenderWindow";
            instance_ = module;
            if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw SdkError{E_FAIL};
            window_ = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, this);
            if (!window_ || !SetTimer(window_, 1, 250, nullptr)) throw SdkError{E_FAIL};
            hook_ = CreatePlayerEvents(engine_, window_);
            Check(dispatcher_->Hook(hook_.get()));
            hooked_ = true;
            commands_ = std::make_unique<RemoteCommands>(window_, player_.get(), dispatcher_.get(), *metadata_, *engine_, remoteDiagnostics_);
            receivers_ = std::make_unique<ReceiverController>(profilePath + L"AirPlay.ini", engine_, commands_->Control());
            pairing_ = std::make_unique<PairingController>(module, *receivers_, engine_, runtime_);
            receivers_->BeforeSelection([this] { pairing_->Cancel(); });
            menu_ = std::make_unique<AirPlayMenu>(core, *receivers_, engine_, dispatcher_.get());
            menu_->Initialize();
            receivers_->Poll();
            menu_->Update(player_->GetState() == AIMP_PLAYER_STATE_PLAYING);
            return S_OK;
        } catch (const SdkError& error) { result = error.code; }
          catch (const std::bad_alloc&) { result = E_OUTOFMEMORY; }
          catch (...) { result = E_FAIL; }
        Finalize();
        return result;
    }
    HRESULT WINAPI Finalize() override {
        if (receivers_) receivers_->CancelDiscovery();
        HRESULT result = menu_ ? menu_->Finalize() : S_OK;
        menu_.reset();
        pairing_.reset();
        receivers_.reset();
        commands_.reset();
        if (hooked_) { dispatcher_->Unhook(hook_.get()); hooked_ = false; }
        if (window_) { KillTimer(window_, 1); DestroyWindow(window_); window_ = nullptr; }
        if (instance_) { UnregisterClassW(L"AimpAirPlaySenderWindow", instance_); instance_ = nullptr; }
        hook_.reset();
        metadata_.reset();
        albumArt_.reset(); player_.reset(); dispatcher_.reset();
        airplay::SetEngine(nullptr);
        if (engine_) engine_->Shutdown();
        engine_.reset(); remoteDiagnostics_.reset(); runtime_.reset(); core_.reset();
        return result;
    }
    void WINAPI SystemNotification(int, IUnknown*) override {}
private:
    ~Plugin() { Finalize(); }
    void Poll() noexcept {
        try {
            if (!menu_) return;
            metadata_->Update();
            pairing_->Poll();
            receivers_->Poll();
            menu_->Update(player_->GetState() == AIMP_PLAYER_STATE_PLAYING);
        } catch (...) { if (menu_) menu_->UpdateError(); }
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<Plugin*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_TIMER && self) { self->Poll(); return 0; }
        if (message == UpdatePlayingFileInfo && self && self->metadata_) {
            try { self->metadata_->Update(true); }
            catch (...) { if (self->menu_) self->menu_->UpdateError(); }
            return 0;
        }
        if (message == WM_APP + 1 && self && self->commands_) { self->commands_->Dispatch(lp); return 0; }
        return DefWindowProcW(window, message, wp, lp);
    }
    std::atomic<ULONG> refs_{1};
    ComPtr<IAIMPCore> core_;
    std::shared_ptr<airplay::CryptoRuntime> runtime_;
    std::shared_ptr<airplay::Engine> engine_;
    std::shared_ptr<airplay::RemoteDiagnostics> remoteDiagnostics_;
    ComPtr<IAIMPServiceMessageDispatcher> dispatcher_;
    ComPtr<IAIMPServicePlayer> player_;
    ComPtr<IAIMPServiceAlbumArt> albumArt_;
    ComPtr<IAIMPMessageHook> hook_;
    std::unique_ptr<PlayerMetadata> metadata_;
    std::unique_ptr<RemoteCommands> commands_;
    std::unique_ptr<ReceiverController> receivers_;
    std::unique_ptr<PairingController> pairing_;
    std::unique_ptr<AirPlayMenu> menu_;
    bool hooked_ = false;
    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
};
}

extern "C" HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    if (!header) return E_POINTER;
    *header = nullptr;
    try { *header = new Plugin; return S_OK; }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_UNEXPECTED; }
}
