#include "pairing_credentials.h"
#include <windows.h>
#include <wincred.h>
#include <algorithm>
namespace airplay {
namespace {
std::wstring CredentialName(std::wstring id) {
    // The mDNS hardware identifier survives changing the receiver's display name.
    auto at = id.find(L'@'); if (at != std::wstring::npos) id.resize(at);
    return L"AIMP/AirPlay/" + id;
}
bool ValidSecret(const std::string& secret) {
    return secret.size() == 64 && std::all_of(secret.begin(), secret.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}
}
std::string LoadPairingSecret(const std::wstring& id) {
    auto name = CredentialName(id); PCREDENTIALW credential = nullptr;
    if (!CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return {};
    std::string secret(reinterpret_cast<char*>(credential->CredentialBlob), credential->CredentialBlobSize); CredFree(credential);
    return ValidSecret(secret) ? secret : std::string();
}
bool SavePairingSecret(const std::wstring& id, const std::string& secret) {
    if (!ValidSecret(secret)) return false;
    auto name = CredentialName(id); CREDENTIALW credential{}; credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = name.data(); credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = DWORD(secret.size()); credential.CredentialBlob = reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()));
    return CredWriteW(&credential, 0) != FALSE;
}
}
