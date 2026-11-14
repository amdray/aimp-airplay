#pragma once
#include <memory>

namespace airplay {
class Engine;
void SetEngine(std::shared_ptr<Engine> engine);
int DspState();
}
