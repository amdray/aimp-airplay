#pragma once
#include "pairing_plist.h"
#include <functional>

namespace airplay::pairing {
// HAP transient M1-M4 authorizes this connection for the legacy RAOP stream.
// The exchange and subsequent RTSP requests must use the same TCP connection.
using PairingPost = std::function<Bytes(const char*, const Bytes&)>;
void TransientPair(const PairingPost& post);
}
