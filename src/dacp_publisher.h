#pragma once
#include <winsock2.h>
#include "remote_control.h"
namespace airplay::dacp {
class Publisher {
public:
    Publisher(std::string id, unsigned short port, RemoteControl::ErrorCallback error);
    ~Publisher();
    void Poll(const std::wstring& target, uint32_t generation);
    in_addr Peer() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
