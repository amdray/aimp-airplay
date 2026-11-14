#pragma once
#include "receivers.h"
#include <atomic>
namespace airplay {
std::vector<Receiver> DiscoverReceivers(int seconds = 4, const std::atomic_bool* cancelled = nullptr);
}
