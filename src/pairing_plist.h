#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace airplay::pairing {
using Bytes = std::vector<unsigned char>;
using Dict = std::map<std::string, Bytes>;
Bytes EncodePlist(const Dict& values, bool strings = false);
Dict DecodePlist(const Bytes& bytes);
}
