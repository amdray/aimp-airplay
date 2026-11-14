#include "airplay_menu.h"
#include <atomic>

namespace airplay::aimp {
namespace {
class Click final : public IAIMPActionEvent {
public:
    Click(std::weak_ptr<std::function<void(const std::wstring&)>> state, std::wstring id) : state_(std::move(state)), id_(std::move(id)) {}
    HRESULT WINAPI QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IAIMPActionEvent)) {
            *out = static_cast<IAIMPActionEvent*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return ++refs_; }
    ULONG WINAPI Release() override { auto n = --refs_; if (!n) delete this; return n; }
    void WINAPI OnExecute(IUnknown*) override {
        if (auto state = state_.lock()) (*state)(id_);
    }
private:
    std::atomic<ULONG> refs_{1};
    std::weak_ptr<std::function<void(const std::wstring&)>> state_;
    std::wstring id_;
};

}
AirPlayMenu::AirPlayMenu(IAIMPCore* core, ReceiverController& receivers, std::shared_ptr<Engine> engine, IAIMPServiceMessageDispatcher* dispatcher)
    : core_(core), receivers_(receivers), engine_(std::move(engine)), dispatcher_(dispatcher),
      actions_(std::make_shared<Actions>([this](const std::wstring& id) { Select(id); })) {}
AirPlayMenu::~AirPlayMenu() { Finalize(); }
void AirPlayMenu::Initialize() {
    ComPtr<IAIMPString> plugins;
    Check(core_->GetPath(AIMP_CORE_PATH_PLUGINS, plugins.put()));
    dspDirectory_.assign(plugins->GetData(), plugins->GetLength());
    if (dspDirectory_.empty()) throw SdkError{E_FAIL};
    if (dspDirectory_.back() != L'\\' && dspDirectory_.back() != L'/') dspDirectory_ += L'\\';
    dspDirectory_ += L"dsp_airplay\\";
    dspMissing_ = DspMissing();
    ComPtr<IAIMPServiceMenuManager> manager;
    Check(core_->QueryInterface(IID_IAIMPServiceMenuManager, manager.out()));
    ComPtr<IAIMPMenuItem> parent;
    Check(manager->GetBuiltIn(AIMP_MENUID_PLAYER_MAIN_FUNCTIONS, parent.put()));
    root_ = Menu(L"aimp.airplay.menu", L"AirPlay", parent.get());
    ComPtr<IAIMPActionEvent> event;
    *event.put() = new Click(actions_, L"!refresh");
    Check(root_->SetValueAsObject(AIMP_MENUITEM_PROPID_EVENT_ONSHOW, event.get()));
    dspMenu_ = Menu(L"aimp.airplay.dsp", L"Настроить передачу звука…", root_.get());
    Check(dspMenu_->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, FALSE));
    event.reset();
    *event.put() = new Click(actions_, L"!dsp");
    Check(dspMenu_->SetValueAsObject(AIMP_MENUITEM_PROPID_EVENT, event.get()));
    hint_ = Menu(L"aimp.airplay.discovery", L"Поиск приёмников…", root_.get());
    Check(hint_->SetValueAsInt32(AIMP_MENUITEM_PROPID_ENABLED, FALSE));
    fixedCount_ = registered_.size();
    BuildChoices();
    receiverRevision_ = receivers_.Revision();
}
HRESULT AirPlayMenu::Finalize() noexcept {
    actions_.reset();
    HRESULT result = S_OK;
    for (auto it = registered_.rbegin(); it != registered_.rend(); ++it) {
        auto hr = core_->UnregisterExtension(it->get());
        if (FAILED(hr) && SUCCEEDED(result)) result = hr;
    }
    registered_.clear(); choices_.clear();
    root_.reset(); hint_.reset(); dspMenu_.reset();
    return result;
}
void AirPlayMenu::Select(const std::wstring& id) {
    try {
        if (id == L"!refresh") { Show(); return; }
        if (!id.empty() && DspMissing()) {
            MessageBoxW(nullptr, L"Не удалось включить передачу звука: AirPlay PCM не установлен. Установите пакет AirPlay PCM и перезапустите AIMP.", L"AirPlay", MB_OK | MB_ICONERROR);
            return;
        }
        if (id == L"!dsp") { Check(dispatcher_->Send(AIMP_MSG_CMD_DSPMANAGER, 0, nullptr)); return; }
        receivers_.Select(id);
        CheckSelection(receivers_.Config().selected);
    } catch (...) {
        try { CheckSelection(receivers_.Config().selected); } catch (...) {}
        MessageBoxW(nullptr, L"Не удалось сохранить выбор AirPlay. Проверьте доступ к файлу AirPlay.ini в профиле AIMP.", L"AirPlay", MB_OK | MB_ICONERROR);
    }
}
void AirPlayMenu::CheckSelection(const std::wstring& id) {
    for (const auto& choice : choices_)
        Check(choice.item->SetValueAsInt32(AIMP_MENUITEM_PROPID_CHECKED, choice.id == id));
}
void AirPlayMenu::Show() {
    dspMissing_ = DspMissing();
    receivers_.Poll();
    Update(playing_);
}
void AirPlayMenu::Update(bool playing) {
    playing_ = playing;
    if (receiverRevision_ != receivers_.Revision()) { BuildChoices(); receiverRevision_ = receivers_.Revision(); }
    CheckSelection(receivers_.Config().selected);
    Text(core_.get(), hint_.get(), AIMP_MENUITEM_PROPID_CAPTION, receivers_.SearchCaption());
    Check(hint_->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, receivers_.Config().receivers.empty()));
    auto now = GetTickCount64();
    Text(core_.get(), dspMenu_.get(), AIMP_MENUITEM_PROPID_CAPTION,
         dspMissing_ ? L"AirPlay PCM не установлен…" : L"Настроить передачу звука…");
    if (!playing || engine_->CaptureAvailable()) dspWaitSince_ = 0;
    else if (!dspWaitSince_) dspWaitSince_ = now;
    Check(dspMenu_->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, dspMissing_ || (dspWaitSince_ && now - dspWaitSince_ >= 5000)));
}
bool AirPlayMenu::DspMissing() const {
    if (engine_->CaptureAvailable()) return false;
    // AIMP installs x64 packages into the plugin's working folder; an unpacked
    // SDK package can also retain the x64 subfolder.
    for (const auto* name : {L"dsp_airplay.dll", L"x64\\dsp_airplay.dll"}) {
        auto attributes = GetFileAttributesW((dspDirectory_ + name).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        }
    }
    return true;
}
void AirPlayMenu::UpdateError() noexcept {
    try { Text(core_.get(), hint_.get(), AIMP_MENUITEM_PROPID_CAPTION, L"Ошибка поиска / обновления списка"); } catch (...) {}
}
void AirPlayMenu::BuildChoices() {
    choices_.clear();
    while (registered_.size() > fixedCount_) {
        Check(core_->UnregisterExtension(registered_.back().get()));
        registered_.pop_back();
    }
    Choice(L"off", L"Локально", L"", root_.get());
    for (const auto& receiver : receivers_.Config().receivers)
        Choice(receiver.id, receiver.name, receiver.id, root_.get());
    CheckSelection(receivers_.Config().selected);
}
void AirPlayMenu::Register(REFIID service, IUnknown* object) {
    // Allocate ownership before registration so rollback also works on allocation failure.
    registered_.emplace_back(object);
    auto hr = core_->RegisterExtension(service, object);
    if (FAILED(hr)) { registered_.pop_back(); throw SdkError{hr}; }
}
ComPtr<IAIMPMenuItem> AirPlayMenu::Menu(const std::wstring& id, const std::wstring& caption, IAIMPMenuItem* parent) {
    ComPtr<IAIMPMenuItem> item;
    Check(core_->CreateObject(IID_IAIMPMenuItem, item.out()));
    Text(core_.get(), item.get(), AIMP_MENUITEM_PROPID_ID, id);
    Text(core_.get(), item.get(), AIMP_MENUITEM_PROPID_CAPTION, caption);
    Check(item->SetValueAsObject(AIMP_MENUITEM_PROPID_PARENT, parent));
    Check(item->SetValueAsInt32(AIMP_MENUITEM_PROPID_ENABLED, TRUE));
    Check(item->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, TRUE));
    Register(IID_IAIMPServiceMenuManager, item.get());
    return item;
}
void AirPlayMenu::Choice(const std::wstring& suffix, const std::wstring& caption, const std::wstring& id, IAIMPMenuItem* root) {
    ComPtr<IAIMPAction> action;
    Check(core_->CreateObject(IID_IAIMPAction, action.out()));
    Text(core_.get(), action.get(), AIMP_ACTION_PROPID_ID, L"aimp.airplay.action." + suffix);
    Text(core_.get(), action.get(), AIMP_ACTION_PROPID_NAME, caption);
    Text(core_.get(), action.get(), AIMP_ACTION_PROPID_GROUPNAME, L"AirPlay");
    Check(action->SetValueAsInt32(AIMP_ACTION_PROPID_ENABLED, TRUE));
    ComPtr<IAIMPActionEvent> event;
    *event.put() = new Click(actions_, id);
    Check(action->SetValueAsObject(AIMP_ACTION_PROPID_EVENT, event.get()));
    Register(IID_IAIMPServiceActionManager, action.get());
    auto item = Menu(L"aimp.airplay.choice." + suffix, caption, root);
    Check(item->SetValueAsObject(AIMP_MENUITEM_PROPID_ACTION, action.get()));
    Check(item->SetValueAsInt32(AIMP_MENUITEM_PROPID_STYLE, AIMP_MENUITEM_STYLE_RADIOBOX));
    choices_.push_back({id, std::move(item)});
}
}
