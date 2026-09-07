// coop/session/host_mode.h -- HOW a hosted session is reachable. Two answers, Direct and Brokered.
// They differ in one thing, who introduces the peers, and the enum below says how each does it.
//
// Whether the session is ANNOUNCED to the master is a SEPARATE axis, `listed`, with its own field.
// There is deliberately no third mode and no accept filter that refuses remote addresses: a port
// that is not forwarded is already local-only, for free, so such a filter could only act on a host
// who had forwarded a port and then asked us to refuse the reachability they had just arranged. Who
// may actually join is the lobby password and the admission challenge, and those apply to every
// lane.
//
// THE INDEPENDENCE PROPERTY, which is the point of the Direct family: a Direct host with
// `listed == false` makes ZERO master calls for the session -- no announce, no heartbeat, no
// signaling; only opening the server browser contacts the master, whatever the mode. `Brokered`
// cannot promise that, because the master is a relay game's only rendezvous.

#pragma once

#include <cstdint>

namespace coop::session {

// HOW friends reach the host. This is the whole axis; there is no second one.
enum class Reachability : uint8_t {
    // P2P through signaling + ICE. The Multivoid server introduces the peers, so it must be
    // reachable -- and in exchange the player forwards nothing.
    Brokered,
    // Our own UDP listen socket, bound on EVERY interface. That single fact covers both
    // cases the old UI split in two: friends on the same network reach it as-is, and
    // friends on the internet reach it if the player forwards the port. No server is
    // involved at any point.
    Direct,
};

// The whole answer to "how is this session reachable".
struct HostMode {
    Reachability reach = Reachability::Brokered;
    // Announce to the master so the session appears in the server browser. INDEPENDENT of
    // the axis above -- it is about DISCOVERY, not about how a peer connects once it knows
    // where to go -- which is exactly why the old third row was a duplicate of it.
    //
    // Forced true on Brokered: an unlisted brokered lobby is unreachable by ANYONE, because
    // the master is the only thing that can hand a joiner the host's identity.
    bool listed = true;
};

// Can this session run without ever contacting the master? The property the Direct family
// exists to provide, stated as a predicate so no caller re-derives it from two fields.
constexpr bool IsMasterFree(const HostMode& m) {
    return m.reach == Reachability::Direct && !m.listed;
}

}  // namespace coop::session
