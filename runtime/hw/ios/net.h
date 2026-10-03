// The network under the guest's /dev/net devices (net.cpp).
#pragma once

namespace guest {

// NOMA_OFFLINE=1 (the default): the console has no network (no link, no address), and the
// channel skips its online steps. NOMA_OFFLINE=0: the internet, through the host's sockets (on
// the 3DS, the SOC service).
bool net_offline();

}  // namespace guest
