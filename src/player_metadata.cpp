#include "player_metadata.h"
#include "protocol_diagnostics.h"
#include <algorithm>
#include <cmath>

namespace airplay::aimp {
namespace {
std::wstring Tag(IAIMPFileInfo* info, int property) {
    ComPtr<IAIMPString> value;
    if (FAILED(info->GetValueAsObject(property, IID_IAIMPString, value.out())) || !value) return {};
    return std::wstring(value->GetData(), value->GetLength());
}
}
PlayerMetadata::PlayerMetadata(IAIMPCore* core, IAIMPServicePlayer* player,
                               IAIMPServiceAlbumArt* albumArt, std::shared_ptr<Engine> engine)
    : core_(core), player_(player), albumArt_(albumArt), engine_(std::move(engine)) {}

PlayerMetadata::~PlayerMetadata() {
    if (coverRequest_ && !coverRequest_->completed.load(std::memory_order_acquire) && albumArt_)
        albumArt_->Cancel(coverTask_, AIMP_SERVICE_ALBUMART_FLAGS_WAITFOR);
}

void PlayerMetadata::ConsumeCover() {
    if (coverRequest_ && coverRequest_->completed.load(std::memory_order_acquire)) {
        if (coverRequest_->trackKey == trackKey_) {
            trackMetadata_.artwork = std::move(coverRequest_->bytes);
            trackMetadata_.coverConsumed = GetTickCount64();
            coverComplete_ = !trackMetadata_.artwork.empty();
            if (coverComplete_) engine_->SetMetadata(trackMetadata_);
        }
        coverRequest_.reset();
        coverTask_ = {};
    }
}
void PlayerMetadata::Update(bool refresh) {
    if (refresh) lastTrackPoll_ = 0;
    int playback = player_->GetState();
    engine_->SetPlaybackState(playback);
    if (playback == AIMP_PLAYER_STATE_STOPPED) {
        trackKey_.clear();
        ConsumeCover();
        return;
    }
    double position = 0, duration = 0;
    player_->GetPosition(&position);
    player_->GetDuration(&duration);
    auto milliseconds = [](double value) -> uint32_t {
        return std::isfinite(value) ? uint32_t(std::clamp(value * 1000.0, 0.0, double(UINT32_MAX))) : 0;
    };
    engine_->SetProgress(milliseconds(position), milliseconds(duration));
    auto tick = GetTickCount64();
    const auto streamStart = detail::protocol.streamStart.load();
    if (!refresh && tick - lastTrackPoll_ < 1000) {
        ConsumeCover();
        return;
    }
    lastTrackPoll_ = tick;
    ComPtr<IAIMPFileInfo> info;
    if (FAILED(player_->GetInfo(info.put())) || !info) return;
    airplay::TrackMetadata track;
    auto filename = Tag(info.get(), AIMP_FILEINFO_PROPID_FILENAME);
    track.title = Tag(info.get(), AIMP_FILEINFO_PROPID_TITLE);
    track.artist = Tag(info.get(), AIMP_FILEINFO_PROPID_ARTIST);
    track.album = Tag(info.get(), AIMP_FILEINFO_PROPID_ALBUM);
    if (track.title.empty()) {
        track.title = filename.substr(filename.find_last_of(L"\\/") + 1);
        auto extension = track.title.find_last_of(L'.');
        if (extension != std::wstring::npos) track.title.resize(extension);
    }
    auto key = filename + L"\n" + track.title + L"\n" + track.artist + L"\n" + track.album;
    bool sameTrack = key == trackKey_;
    if (sameTrack && (coverComplete_ || coverAttempts_ >= 3)) {
        ConsumeCover();
        return;
    }
    if (!sameTrack) {
        coverAttempts_ = 0;
        coverComplete_ = false;
        trackMetadata_ = std::move(track);
        trackMetadata_.streamStart = streamStart;
        trackMetadata_.detected = GetTickCount64();
        engine_->SetMetadata(trackMetadata_);
    }
    trackKey_ = std::move(key);
    ConsumeCover();
    if (albumArt_ && !coverRequest_) {
        ++coverAttempts_;
        coverRequest_ = std::make_unique<CoverRequest>();
        coverRequest_->core = core_.get();
        coverRequest_->trackKey = trackKey_;
        // The request owns callback data until completion; only shutdown waits for cancellation.
        if (FAILED(albumArt_->Get2(info.get(), AIMP_SERVICE_ALBUMART_FLAGS_OFFLINE, ReceiveCoverAsync, coverRequest_.get(), &coverTask_))) {
            coverRequest_.reset();
            coverTask_ = {};
        }
    }
}
}
