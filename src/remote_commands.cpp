#include "remote_commands.h"

namespace airplay::aimp {
namespace {
const char* CommandName(airplay::RemoteCommand command) {
    switch (command) {
    case airplay::RemoteCommand::PlayPause: return "playpause";
    case airplay::RemoteCommand::Play: return "play";
    case airplay::RemoteCommand::Pause: return "pause";
    case airplay::RemoteCommand::Stop: return "stop";
    case airplay::RemoteCommand::Next: return "nextitem";
    case airplay::RemoteCommand::Previous: return "previtem";
    case airplay::RemoteCommand::Restart: return "restartitem";
    }
    return "unknown";
}

}
RemoteCommands::RemoteCommands(HWND window, IAIMPServicePlayer* player, IAIMPServiceMessageDispatcher* dispatcher,
                               PlayerMetadata& metadata, Engine& engine,
                               std::shared_ptr<RemoteDiagnostics> diagnostics)
    : window_(window), player_(player), dispatcher_(dispatcher), metadata_(metadata), diagnostics_(std::move(diagnostics)) {
    remote_ = std::make_unique<RemoteControl>([window, diagnostics = diagnostics_](RemoteCommand command, uint32_t generation) {
        auto event = std::make_unique<Event>(Event{command, generation});
        if (PostMessageW(window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(event.get()))) event.release();
        else diagnostics->Result(CommandName(command), HRESULT_FROM_WIN32(GetLastError()));
    }, [diagnostics = diagnostics_](const std::string& stage, const std::string& detail) {
        diagnostics->Error(stage, detail);
    });
    engine.SetRemoteIdentity(remote_->Id(), remote_->Token());
}
RemoteCommands::~RemoteCommands() {
    remote_.reset();
    MSG pending{};
    while (window_ && PeekMessageW(&pending, window_, WM_APP + 1, WM_APP + 1, PM_REMOVE))
        delete reinterpret_cast<Event*>(pending.lParam);
}
void RemoteCommands::Dispatch(LPARAM message) {
    std::unique_ptr<Event> event(reinterpret_cast<Event*>(message));
    if (!event) return;
    if (remote_ && remote_->IsCurrent(event->generation)) Execute(*event);
    else diagnostics_->Error("stale_remote_command", CommandName(event->command));
}
void RemoteCommands::Execute(const Event& event) {
    HRESULT result = E_FAIL;
    try {
        switch (event.command) {
        case airplay::RemoteCommand::PlayPause:
            result = dispatcher_->Send(AIMP_MSG_CMD_PLAYPAUSE, 0, nullptr);
            break;
        case airplay::RemoteCommand::Play:
            result = S_OK;
            if (player_->GetState() == AIMP_PLAYER_STATE_PAUSED) result = player_->Resume();
            else if (player_->GetState() == AIMP_PLAYER_STATE_STOPPED) result = player_->Play(nullptr);
            break;
        case airplay::RemoteCommand::Pause:
            result = player_->Pause();
            break;
        case airplay::RemoteCommand::Stop:
            result = player_->Stop();
            break;
        case airplay::RemoteCommand::Next:
            result = player_->GoToNext();
            if (SUCCEEDED(result)) metadata_.Update(true);
            break;
        case airplay::RemoteCommand::Previous:
            result = player_->GoToPrev();
            if (SUCCEEDED(result)) metadata_.Update(true);
            break;
        case airplay::RemoteCommand::Restart:
            result = player_->SetPosition(0);
            if (SUCCEEDED(result)) metadata_.Update(true);
            break;
        }
    } catch (...) {}
    diagnostics_->Result(CommandName(event.command), result);
}
}
