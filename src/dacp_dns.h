#pragma once
#include <winsock2.h>
#include <string>
#include <vector>
namespace airplay::dacp {
using Bytes = std::vector<unsigned char>;
Bytes Advertisement(const std::string& id, unsigned short port, in_addr local);
bool RelevantQuery(const unsigned char* bytes, size_t size, const std::string& id, bool* unicast = nullptr);
}
