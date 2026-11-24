// coop/props/rdrone_sync.h -- the remote drone is the host's: where it flies, whether it is on, and its energy; and a
// ridden sled is its rider's to sync, granted by the host.
//
// The drone (prop_rdrone_C) hovers by its own tick while it is `active`, its forces and torque turning it toward its
// `look`, and drains its `energy` (charged by its cord); the laptop's drone app flies it through `move(forward, rot,
// up)`, one frame's force per slider change, and shows its camera. Each peer ran that on its own copy, so a client's
// flight was its own. Here a client refuses the drone's tick, so only the host's copy hovers. A client's `move` is
// asked of the host (RdroneAsk), its yaw also turned on the client's own copy so the pilot's view answers at once; the
// asks are summed and sent at most every 50 ms, and the host runs them on its copy. A client's press of the switch
// (action 8) is asked too, judged by reach. The host claims a running drone's drive, so every copy follows its pose on
// the drive stream, and sends its `active` and its energy in 5% steps (RdroneState), which a client writes and shows
// through the drone's own `upd`; a joiner gets every drone at its ready edge.
// The sled (prop_sled_C) is pushed by its own tick from its rider's keys. The rider's machine claims it through the
// prop syncer (coop/props/prop_syncer, reason Ride) every tick it rides, so the host grants it and its pose streams to
// every peer; on the host that is the host's own drive. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct RdroneStatePayload;
struct RdroneAskPayload;
}  // namespace coop::net

namespace coop::rdrone_sync {

// Keep the session and register the watches on the drone and the sled. Idempotent.
void Install(coop::net::Session* session);

// Settles the watches; CLIENT: sends the summed flight asks; every peer: claims the sled its player rides. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every drone's state, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's ask, its length checked by the dispatcher.
void OnAsk(int senderSlot, const coop::net::RdroneAskPayload& p);

// CLIENT: the host's state, its length checked by the dispatcher.
void OnState(const coop::net::RdroneStatePayload& p);

// CLIENT (coop/props/remote_prop): a prop copy was bound to element id `eid`; a state held for it is applied.
void OnBound(uint32_t eid, void* actor);

// CLIENT: the world changed under the session: the states held, the asks summed and the ridden sled go.
void OnClientWorldReady();

// Session end.
void OnDisconnect();

}  // namespace coop::rdrone_sync
