// The Sound Player's stage (see stage.h).
#include "engine/soundplayer/stage.h"

namespace engine::soundplayer {

struct Stage::Impl {};

Stage::Stage() : impl_(new Impl) {}
Stage::~Stage() { delete impl_; }
void Stage::update(const Music&) {}
void Stage::draw() {}

}  // namespace engine::soundplayer
