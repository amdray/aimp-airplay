#pragma once
#include "apiPlugin.h"
#include "com_ptr.h"
#include <string>

namespace airplay::aimp {
struct SdkError { HRESULT code; };
inline void Check(HRESULT code) { if (FAILED(code)) throw SdkError{code}; }
inline ComPtr<IAIMPString> String(IAIMPCore* core, const std::wstring& text) {
    ComPtr<IAIMPString> value;
    Check(core->CreateObject(IID_IAIMPString, value.out()));
    Check(value->SetData(const_cast<wchar_t*>(text.c_str()), static_cast<int>(text.size())));
    return value;
}
inline void Text(IAIMPCore* core, IAIMPPropertyList* object, int property, const std::wstring& text) {
    auto value = String(core, text);
    Check(object->SetValueAsObject(property, value.get()));
}
}
