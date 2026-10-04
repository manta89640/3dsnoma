// The Sound Player's list of tracks (see track_list.h).
#include "engine/soundplayer/track_list.h"

namespace engine::soundplayer {

struct TrackList::Impl {
    int chosen = -1;
};

TrackList::TrackList(u32, u32) : impl_(new Impl) {}
TrackList::~TrackList() { delete impl_; }
void TrackList::update(const Music&) {}
void TrackList::draw() {}
bool TrackList::on_pushed(u32) { return false; }
int TrackList::take_chosen() {
    int c = impl_->chosen;
    impl_->chosen = -1;
    return c;
}
int TrackList::highlighted() const { return 0; }

}  // namespace engine::soundplayer
