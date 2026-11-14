#include "player_events.h"
#include "protocol_diagnostics.h"
#include <atomic>

namespace airplay::aimp {
namespace {
class PlayerEvents final : public IAIMPMessageHook {
public:
    PlayerEvents(std::weak_ptr<airplay::Engine> engine, HWND window) : engine_(std::move(engine)), window_(window) {}
    HRESULT WINAPI QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IAIMPMessageHook)) {
            *out = static_cast<IAIMPMessageHook*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return ++refs_; }
    ULONG WINAPI Release() override { auto n = --refs_; if (!n) delete this; return n; }
    void WINAPI CoreMessage(LongWord message, int param, void*, HRESULT*) override {
        try {
            if (message == AIMP_MSG_EVENT_STREAM_START || message == AIMP_MSG_EVENT_STREAM_START_SUBTRACK)
                detail::protocol.streamStart = GetTickCount64();
            if (message == AIMP_MSG_EVENT_PLAYING_FILE_INFO)
                PostMessageW(window_, UpdatePlayingFileInfo, 0, 0);
            if (message == AIMP_MSG_EVENT_TERMINATING)
                detail::RecordPlaybackEvent("terminating", -1);
            if (message == AIMP_MSG_EVENT_PLAYER_STATE)
                if (auto engine = engine_.lock()) engine->SetPlaybackState(param);
            if (message == AIMP_MSG_EVENT_PROPERTY_VALUE && param == AIMP_MSG_PROPERTY_PLAYER_POSITION)
                if (auto engine = engine_.lock()) engine->Flush();
        } catch (...) {}
    }
private:
    std::atomic<ULONG> refs_{1};
    std::weak_ptr<airplay::Engine> engine_;
    HWND window_;
};

}
ComPtr<IAIMPMessageHook> CreatePlayerEvents(std::shared_ptr<Engine> engine, HWND window) {
    ComPtr<IAIMPMessageHook> hook;
    *hook.put() = new PlayerEvents(std::move(engine), window);
    return hook;
}
}
