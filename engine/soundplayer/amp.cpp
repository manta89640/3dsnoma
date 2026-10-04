// The amp on the Sound Player's stage (see amp.h).
#include "engine/soundplayer/amp.h"

namespace engine::soundplayer {

struct Amp::Impl {};

Amp::Amp() : impl_(new Impl) {}
Amp::~Amp() { delete impl_; }
void Amp::draw(u32, const float*, float, float) {}

}  // namespace engine::soundplayer
