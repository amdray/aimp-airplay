#pragma once
#include <memory>
#include <string>
#include <vector>
namespace airplay::pairing {
class Transport {
public:
    Transport(std::wstring host, unsigned short port);
    ~Transport();
    void Connect();
    std::vector<unsigned char> Post(const char* path, const std::vector<unsigned char>& body);
    void Cancel() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
