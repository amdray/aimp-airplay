#pragma once
#include "player_metadata.h"
#include "remote_control.h"
#include "remote_diagnostics.h"
#include "apiMessages.h"

namespace airplay::aimp {
class RemoteCommands {
public:
    RemoteCommands(HWND window, IAIMPServicePlayer* player,
                   IAIMPServiceMessageDispatcher* dispatcher,
                   PlayerMetadata& metadata, Engine& engine,
                   std::shared_ptr<RemoteDiagnostics> diagnostics);
    ~RemoteCommands();
    void Dispatch(LPARAM message);
    RemoteControl& Control() const { return *remote_; }
private:
    struct Event { RemoteCommand command; uint32_t generation; };
    void Execute(const Event& event);
    HWND window_;
    ComPtr<IAIMPServicePlayer> player_;
    ComPtr<IAIMPServiceMessageDispatcher> dispatcher_;
    PlayerMetadata& metadata_;
    std::shared_ptr<RemoteDiagnostics> diagnostics_;
    std::unique_ptr<RemoteControl> remote_;
};
}
