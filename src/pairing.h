#pragma once
#include "receivers.h"
#include <memory>

namespace airplay {
class CryptoRuntime;
// A session owns its TCP connection between displaying and confirming the PIN.
class PinPairing {
public:
    PinPairing(const Receiver& receiver, std::shared_ptr<CryptoRuntime> runtime);
    ~PinPairing();
    void Start();
    std::string Finish(const std::string& pin);
    void Cancel() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
