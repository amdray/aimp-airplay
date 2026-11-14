#include <windows.h>
#include "dsp_bridge.h"
#include "engine.h"
#include <mutex>
#include <utility>

namespace {
std::mutex bridgeMutex;
std::shared_ptr<airplay::Engine> currentEngine;
int dspState = 0;
std::shared_ptr<airplay::Engine> CurrentEngine() {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    return currentEngine;
}
}

namespace airplay {
void SetEngine(std::shared_ptr<Engine> engine) {
    std::shared_ptr<Engine> previous;
    {
        std::lock_guard<std::mutex> lock(bridgeMutex);
        if (engine) engine->SetCaptureAvailable(dspState == 1);
        previous = std::exchange(currentEngine, std::move(engine));
    }
}
int DspState() {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    return dspState;
}
}

extern "C" __declspec(dllexport) void WINAPI AimpAirPlayDSPState(int state) {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    dspState = state;
    if (currentEngine) currentEngine->SetCaptureAvailable(state == 1);
}
extern "C" __declspec(dllexport) void WINAPI AimpAirPlayPushPCM(const void* samples, int frames, int bits, int channels, int rate) {
    if (auto engine = CurrentEngine()) engine->Push(samples, frames, bits, channels, rate);
}
extern "C" __declspec(dllexport) BOOL WINAPI AimpAirPlayRemoteOutputActive() {
    if (auto engine = CurrentEngine()) return engine->MuteLocalOutput();
    return FALSE;
}
