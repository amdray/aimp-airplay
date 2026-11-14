#include <windows.h>
#include <atomic>
#include <cstring>

// Winamp DSP v0.20 ABI: callbacks use cdecl, including on 32-bit builds.
struct DspModule {
    char* description;
    HWND hwndParent;
    HINSTANCE hDllInstance;
    void (*Config)(DspModule*);
    int (*Init)(DspModule*);
    int (*ModifySamples)(DspModule*, short*, int, int, int, int);
    void (*Quit)(DspModule*);
    void* userData;
};
struct DspHeader { int version; char* description; DspModule* (*getModule)(int); };
namespace {
using PushProc = void (WINAPI*)(const void*, int, int, int, int);
using StateProc = void (WINAPI*)(int);
using RemoteProc = BOOL (WINAPI*)();
HMODULE addon = nullptr;
PushProc push = nullptr;
StateProc state = nullptr;
RemoteProc remote = nullptr;
std::atomic<unsigned long> bindError{0}, bound{0};
ULONGLONG nextBind = 0;
void Config(DspModule* module) {
    MessageBoxW(module->hwndParent, L"Выберите приёмник в главном меню AIMP → AirPlay.\nЭтот DSP передаёт PCM выбранному приёмнику.", L"AirPlay", MB_OK);
}
bool Bind() {
    // Pin the add-on while DSP callbacks refer to its exported code.
    if (!GetModuleHandleExW(0, L"aimp_airplay.dll", &addon)) { bindError = GetLastError(); return false; }
    push = reinterpret_cast<PushProc>(GetProcAddress(addon, "AimpAirPlayPushPCM"));
    state = reinterpret_cast<StateProc>(GetProcAddress(addon, "AimpAirPlayDSPState"));
    remote = reinterpret_cast<RemoteProc>(GetProcAddress(addon, "AimpAirPlayRemoteOutputActive"));
    if (!push || !state || !remote) { bindError = ERROR_PROC_NOT_FOUND; push = nullptr; state = nullptr; remote = nullptr; FreeLibrary(addon); addon = nullptr; return false; }
    bindError = 0; bound = 1;
    if (state) state(1);
    return true;
}
int Init(DspModule*) {
    nextBind = 0;
    if (!push) Bind();
    // AIMP may initialize DSP before loading the menu add-on. Stay available
    // and attach from a later audio callback instead of failing permanently.
    return 0;
}
int Modify(DspModule*, short* samples, int frames, int bits, int channels, int rate) {
    if (!push && GetTickCount64() >= nextBind) { nextBind = GetTickCount64() + 1000; Bind(); }
    if (push) push(samples, frames, bits, channels, rate);
    // Push copies the original PCM before local playback is silenced.
    // Keep local output available while disconnected or on unsupported input.
    if (push && remote && remote() && samples && frames > 0 && frames <= 262144 &&
        (bits == 16 || bits == 24 || bits == 32) && (channels == 1 || channels == 2) && rate >= 8000 && rate <= 384000)
        std::memset(samples, 0, static_cast<size_t>(frames) * channels * (bits / 8));
    return frames;
}
void Quit(DspModule*) {
    if (state) state(2);
    // AIMP may restart this module between tracks. DSPState lets the engine
    // release a genuinely disabled DSP after its transition grace period.
    push = nullptr;
    state = nullptr;
    remote = nullptr;
    if (addon) FreeLibrary(addon);
    addon = nullptr;
    bound = 0;
}
DspModule module{const_cast<char*>("AirPlay PCM"), nullptr, nullptr, Config, Init, Modify, Quit, nullptr};
DspModule* GetModule(int index) { return index == 0 ? &module : nullptr; }
DspHeader header{0x20, const_cast<char*>("AirPlay PCM sender"), GetModule};
}
extern "C" __declspec(dllexport) DspHeader* winampDSPGetHeader2() { return &header; }
extern "C" __declspec(dllexport) DspHeader* winampDSPGetHeader() { return &header; }
extern "C" __declspec(dllexport) void WINAPI AirPlayDSPStatus(unsigned long* values) {
    if (!values) return;
    values[0] = bindError.load(); values[1] = bound.load();
}
