#include "receivers.h"
#include <windows.h>
#include <stdexcept>
#include <mutex>

namespace airplay {
namespace {
std::mutex iniMutex;
bool EnsureUnicodeIni(const std::wstring& path) {
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    std::vector<char> bytes;
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart > 256 * 1024) { CloseHandle(file); return false; }
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD count = 0;
        bool ok = bytes.empty() || (ReadFile(file, bytes.data(), DWORD(bytes.size()), &count, nullptr) && count == bytes.size());
        CloseHandle(file);
        if (!ok) return false;
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe) return true;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    int length = bytes.empty() ? 0 : MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    if (!bytes.empty() && !length) return false;
    std::wstring text(size_t(length) + 1, L'\0');
    text[0] = 0xfeff;
    if (length) MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(bytes.size()), text.data() + 1, length);
    auto temporary = path + L".unicode.tmp";
    file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD count = 0, size = DWORD(text.size() * sizeof(wchar_t));
    bool ok = WriteFile(file, text.data(), size, &count, nullptr) && count == size && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return ok;
}
std::wstring Read(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[1024]{};
    auto size = GetPrivateProfileStringW(section, key, L"", buffer, 1024, path.c_str());
    if (size == 1023) throw std::runtime_error("INI value too long");
    return {buffer, size};
}
unsigned short Port(const std::wstring& text) {
    unsigned value = 0;
    if (text.empty() || text.size() > 5) throw std::runtime_error("Invalid receiver port");
    for (auto ch : text) {
        if (ch < L'0' || ch > L'9') throw std::runtime_error("Invalid receiver port");
        value = value * 10 + (ch - L'0');
    }
    if (!value || value > 65535) throw std::runtime_error("Invalid receiver port");
    return static_cast<unsigned short>(value);
}
}
ReceiverConfig LoadReceivers(const std::wstring& path) {
    ReceiverConfig result;
    // A stale or partially written cache must not prevent the plugin loading.
    try {
        auto id = Read(path, L"LastReceiver", L"Id");
        if (id.compare(0, 5, L"mdns:") == 0) {
            Receiver cached{id, Read(path, L"LastReceiver", L"Name"), Read(path, L"LastReceiver", L"Host"),
                            Port(Read(path, L"LastReceiver", L"Port"))};
            auto narrow = [&](const wchar_t* key) { auto value = Read(path, L"LastReceiver", key); return std::string(value.begin(), value.end()); };
            cached.encryption = narrow(L"Encryption");
            cached.model = narrow(L"Model");
            cached.metadata = narrow(L"Metadata");
            cached.features = narrow(L"Features");
            cached.pairingFlags = narrow(L"PairingFlags");
            cached.passwordRequired = Read(path, L"LastReceiver", L"PasswordRequired") == L"1";
            if (!cached.name.empty() && !cached.host.empty())
                result.receivers.push_back(std::move(cached));
        }
    } catch (const std::runtime_error&) {}
    result.selected = Read(path, L"AirPlay", L"Selected");
    if (result.selected.compare(0, 5, L"mdns:") != 0) result.selected.clear();
    return result;
}
bool SaveSelection(const std::wstring& path, const std::wstring& id) {
    std::lock_guard<std::mutex> lock(iniMutex);
    if (!EnsureUnicodeIni(path)) return false;
    return WritePrivateProfileStringW(L"AirPlay", L"Selected", id.c_str(), path.c_str()) != FALSE;
}
bool SaveReceiverCache(const std::wstring& path, const Receiver& receiver) {
    if (receiver.id.compare(0, 5, L"mdns:") != 0) return true;
    std::lock_guard<std::mutex> lock(iniMutex);
    if (!EnsureUnicodeIni(path)) return false;
    auto write = [&](const wchar_t* key, const std::wstring& value) {
        return WritePrivateProfileStringW(L"LastReceiver", key, value.c_str(), path.c_str()) != FALSE;
    };
    auto wide = [](const std::string& value) { return std::wstring(value.begin(), value.end()); };
    // Commit identity last so an incomplete update is not loaded as a receiver.
    return write(L"Id", L"") && write(L"Name", receiver.name) && write(L"Host", receiver.host) &&
           write(L"Port", std::to_wstring(receiver.port)) && write(L"Encryption", wide(receiver.encryption)) &&
           write(L"Model", wide(receiver.model)) && write(L"Metadata", wide(receiver.metadata)) &&
           write(L"Features", wide(receiver.features)) && write(L"PairingFlags", wide(receiver.pairingFlags)) &&
           write(L"PasswordRequired", receiver.passwordRequired ? L"1" : L"0") && write(L"Id", receiver.id);
}
}
