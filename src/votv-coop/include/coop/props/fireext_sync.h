// coop/props/fireext_sync.h -- a fire extinguisher's foam (prop_fireExt_C) is the host's and every peer sees it.
//
// The extinguisher sprays two ways, each run only on the machine whose copy fires. In a player's hand, its `fire`
// (the hand's LMB press and release) turns its spray effect (eff_ext) and its sound on, and its tick aims the spray
// along that machine's camera, drains `life` and casts `extinguishRay`, which puts out what it touches. In the world,
// an impact over 5 or a fire's damage runs its `shoot`, the runaway thrust: its thruster, its thrust effect
// (eff_fireextthr) and sound, and its own extinguishing rays, until the charge is gone. The other players saw neither.
//
// Here the host owns both. A player's spray is that player's to start and stop: its own copy sprays, and the change
// goes to the host (FireExtIntent), which checks the sender's hand holds an extinguisher, shows the spray on its mirror
// of that hand, casts the game's own extinguishing ray along the sender's aim while it runs, and tells every peer
// (FireExtState op 1), each showing it on its mirror of that hand. A client's own ray is refused, so a fire goes out
// only on the host, whose fire lanes carry it. The thrust is the host's prop's: a client refuses `shoot`, and the host
// sends each world extinguisher's thrust and charge when they change and to a joiner (op 0); a client's copy shows the
// thrust's effect and sound and takes the charge. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct FireExtPayload;
}  // namespace coop::net

namespace coop::fireext_sync {

// Keep the session and register the watches on the extinguisher's fire, thrust and ray. Idempotent.
void Install(coop::net::Session* session);

// HOST: the sprays' rays and each extinguisher's thrust changes. BOTH: the mirrors' spray aim. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every thrusting extinguisher and every spray, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's spray, its length checked by the dispatcher.
void OnIntent(int senderSlot, const coop::net::FireExtPayload& p);

// CLIENT: the host's thrust or a player's spray, its length checked by the dispatcher.
void OnState(const coop::net::FireExtPayload& p);

// A peer left: its spray ends here.
void OnPeerLeft(uint8_t slot);

// Session end: the sprays and records are cleared.
void OnDisconnect();

}  // namespace coop::fireext_sync
