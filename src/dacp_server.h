#pragma once
#include <winsock2.h>
#include "remote_control.h"
#include <atomic>
namespace airplay::dacp {
class Server {
public:
    enum class DispatchResult { Rejected, UnknownCommand, Accepted };
    using Dispatch = std::function<DispatchResult(const RemoteCommand*, uint32_t)>;
    Server(std::string token, const std::atomic<bool>& stopping, Dispatch dispatch, RemoteControl::ErrorCallback error);
    ~Server();
    unsigned short Port() const;
    void Poll(in_addr peer, uint32_t generation);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
