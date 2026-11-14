#pragma once
#include <windows.h>
#include <string>

namespace airplay {
// Shared by the sender and PIN sessions: upstream uses one OpenSSL function table.
class CryptoRuntime {
public:
    explicit CryptoRuntime(const std::wstring& directory);
    ~CryptoRuntime();
    CryptoRuntime(const CryptoRuntime&) = delete;
    CryptoRuntime& operator=(const CryptoRuntime&) = delete;
private:
    HMODULE crypto_ = nullptr;
    HMODULE ssl_ = nullptr;
};
}
