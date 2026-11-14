#pragma once
#include "com_ptr.h"
#include "engine.h"
#include "apiMessages.h"

namespace airplay::aimp {
inline constexpr UINT UpdatePlayingFileInfo = WM_APP + 2;
ComPtr<IAIMPMessageHook> CreatePlayerEvents(std::shared_ptr<Engine> engine, HWND window);
}
