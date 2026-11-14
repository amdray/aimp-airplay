#pragma once
#include "album_art.h"
#include "com_ptr.h"
#include "engine.h"
#include "apiPlayer.h"

namespace airplay::aimp {
// Used on AIMP's main thread; owns pending artwork callback data until completion.
class PlayerMetadata {
public:
    PlayerMetadata(IAIMPCore* core, IAIMPServicePlayer* player,
                   IAIMPServiceAlbumArt* albumArt, std::shared_ptr<Engine> engine);
    ~PlayerMetadata();
    void Update(bool refresh = false);
private:
    void ConsumeCover();
    ComPtr<IAIMPCore> core_;
    ComPtr<IAIMPServicePlayer> player_;
    ComPtr<IAIMPServiceAlbumArt> albumArt_;
    std::shared_ptr<Engine> engine_;
    std::wstring trackKey_;
    airplay::TrackMetadata trackMetadata_;
    std::unique_ptr<CoverRequest> coverRequest_;
    TTaskHandle coverTask_{};
    unsigned coverAttempts_ = 0;
    bool coverComplete_ = false;
    ULONGLONG lastTrackPoll_ = 0;
};
}
