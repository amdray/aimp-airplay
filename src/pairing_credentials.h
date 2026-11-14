#pragma once
#include <string>
namespace airplay {
std::string LoadPairingSecret(const std::wstring& receiverId);
bool SavePairingSecret(const std::wstring& receiverId, const std::string& secret);
}
