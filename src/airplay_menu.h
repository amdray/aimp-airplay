#pragma once
#include "aimp_sdk_helpers.h"
#include "apiMenu.h"
#include "apiMessages.h"
#include "receiver_controller.h"

namespace airplay::aimp {
class AirPlayMenu {
public:
    AirPlayMenu(IAIMPCore* core, ReceiverController& receivers,
                std::shared_ptr<Engine> engine, IAIMPServiceMessageDispatcher* dispatcher);
    ~AirPlayMenu();
    void Initialize();
    void Update(bool playing);
    void Show();
    void UpdateError() noexcept;
    HRESULT Finalize() noexcept;
private:
    using Actions = std::function<void(const std::wstring&)>;
    void Select(const std::wstring& id);
    void CheckSelection(const std::wstring& id);
    void BuildChoices();
    bool DspMissing() const;
    void Register(REFIID service, IUnknown* object);
    ComPtr<IAIMPMenuItem> Menu(const std::wstring& id, const std::wstring& caption, IAIMPMenuItem* parent);
    void Choice(const std::wstring& suffix, const std::wstring& caption, const std::wstring& id, IAIMPMenuItem* root);
    ComPtr<IAIMPCore> core_;
    ReceiverController& receivers_;
    std::shared_ptr<Engine> engine_;
    ComPtr<IAIMPServiceMessageDispatcher> dispatcher_;
    std::shared_ptr<Actions> actions_;
    struct MenuChoice { std::wstring id; ComPtr<IAIMPMenuItem> item; };
    std::vector<MenuChoice> choices_;
    std::vector<ComPtr<IUnknown>> registered_;
    ComPtr<IAIMPMenuItem> root_, hint_, dspMenu_;
    uint64_t receiverRevision_ = UINT64_MAX, dspWaitSince_ = 0;
    size_t fixedCount_ = 0;
    bool playing_ = false;
    bool dspMissing_ = false;
    std::wstring dspDirectory_;
};
}
