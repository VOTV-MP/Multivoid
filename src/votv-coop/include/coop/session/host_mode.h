// coop/session/host_mode.h -- HOW a hosted session is reachable. Two answers, because
// measurement showed there were never three.
//
// WHAT THIS REPLACED, and why it is smaller. Hosting used to be three booleans --
// `directConnection`, `hideFromBrowser`, `lanOnly` -- and three UI rows labelled AUTO /
// DIRECT / LAN ONLY. That presentation says the three are three TRANSPORTS. Measured
// 2026-09-01, they are not:
//
//   `[V]` DIRECT and LAN ONLY called the SAME `Session::StartLanDirect`, did the same
//   `addr.Clear()`, and bound the same socket. Live reading of the host's own endpoints:
//   `:::47621` -- the any-address, every interface, dual-stack (which is why an IPv4 peer
//   dialling 127.0.0.1 reaches it in every LAN run we do). There was NO bind difference
//   between them, and neither was ever loopback-only.
//
// So "LAN ONLY" was two things, neither of them a transport: an ACCEPT FILTER that refused
// remotes outside loopback / RFC1918 / link-local / ULA, and never announcing to the
// master. The second is the `listed` axis, which already exists as its own control. The
// first is DELETED, and the reason is the user's (2026-09-01):
//
//   "our job is to open session, what they want to restrict is their job on the router".
//
// That is right, and the sharpest form of it is physical: IF THE PORT IS NOT FORWARDED,
// LOCAL-ONLY IS WHAT YOU ALREADY HAVE -- NAT does it, for free, with no help from us. The
// filter could therefore only ever act on a host who had DELIBERATELY forwarded a port and
// then asked us to refuse the reachability they had just arranged. Its stated purpose ("a
// forwarded port cannot quietly turn a LAN party into an internet host") describes someone
// who forwarded a port for something else and forgot -- and the honest control for that is
// the lobby password and the admission challenge, both of which apply to every lane. A
// filter that sits in front of them adds no boundary; it only adds a third name for a thing
// that was never a third thing.
//
// THE INDEPENDENCE PROPERTY, which is the point of the Direct family: a Direct host with
// `listed == false` makes ZERO master calls -- no announce, no heartbeat, no signaling.
// `Brokered` cannot make that promise, because the master is a relay game's only rendezvous.

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
