// ue_wrap/devices/serverbox.h -- the signal server (AserverBox_C) engine wrapper.
//
// What the wrapper reaches: mainGamemode_C.servers, whose INDEX is the identity every server lane
// addresses a box by. The level places the boxes, so both peers build the same ordering, and the
// lane that mirrors the break-and-fix simulation relies on exactly that.
//
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include <vector>

namespace ue_wrap::serverbox {

// The gamemode's server list, in its own order. Returns the number appended, 0 before the world
// has a gamemode. Resolves what it needs itself.
size_t ReadServers(std::vector<void*>& out);

}  // namespace ue_wrap::serverbox
