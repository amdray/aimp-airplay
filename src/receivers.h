#pragma once
#include <string>
#include <vector>

namespace airplay {
struct Receiver {
    std::wstring id; // INI section, stable across renaming the display name
    std::wstring name;
    std::wstring host;
    unsigned short port;
    std::string encryption;
    std::string model;
    bool passwordRequired = false;
    std::string metadata = "0,1,2";
    std::string pairingSecret; // Loaded from Windows Credential Manager; never written to INI.
    std::string features;
    std::string pairingFlags;
};
struct ReceiverConfig {
    std::vector<Receiver> receivers;
    std::wstring selected;
};
ReceiverConfig LoadReceivers(const std::wstring& path);
bool SaveSelection(const std::wstring& path, const std::wstring& id);
bool SaveReceiverCache(const std::wstring& path, const Receiver& receiver);
}
