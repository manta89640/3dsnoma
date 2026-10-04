// The Sound Player's controls and title (see controls.h).
#include "engine/soundplayer/controls.h"

namespace engine::soundplayer {

struct Controls::Impl {
    Action action = NONE;
};

Controls::Controls(u32, u32) : impl_(new Impl) {}
Controls::~Controls() { delete impl_; }
void Controls::update(const Music&, const Track*) {}
void Controls::draw() {}
bool Controls::on_pushed(u32) { return false; }
Controls::Action Controls::take_action() {
    Action a = impl_->action;
    impl_->action = NONE;
    return a;
}

}  // namespace engine::soundplayer
