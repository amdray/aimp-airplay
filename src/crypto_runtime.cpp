#include "crypto_runtime.h"
extern "C" {
#include "cross_ssl.h"
#include "cross_log.h"
log_level util_loglevel = lERROR;
log_level raop_loglevel = lERROR;
}
#include <mutex>
#include <stdexcept>

namespace {
std::mutex runtimeMutex;
bool runtimeActive = false;
}

namespace airplay {
CryptoRuntime::CryptoRuntime(const std::wstring& directory) {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    if (runtimeActive)
        throw std::logic_error("Share the existing CryptoRuntime with all AirPlay components");
    crypto_ = LoadLibraryExW((directory + L"\\libcrypto-3-x64.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    ssl_ = LoadLibraryExW((directory + L"\\libssl-3-x64.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!crypto_ || !ssl_ || !cross_ssl_load()) {
        if (ssl_) FreeLibrary(ssl_);
        if (crypto_) FreeLibrary(crypto_);
        throw std::runtime_error("OpenSSL runtime missing");
    }
    runtimeActive = true;
}
CryptoRuntime::~CryptoRuntime() {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    cross_ssl_free();
    FreeLibrary(ssl_);
    FreeLibrary(crypto_);
    runtimeActive = false;
}
}
