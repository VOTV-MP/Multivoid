// coop/props/prop_switch_sync.h -- a store light's or the hot plate's on switch is the host's.
//
// The desk lamp, the floor lamp, the wall lights, the spotlight's head (alone or on its tripod) and the hot plate
// flip their own `active` and run their `upd`, which shows the light, the glow or the heat (the hot plate's heat level,
// its `power`, is stepped the same way and carried with its switch); the smoke detector sounds through `alarm`, which a
// client's own fire check never runs (it is refused: the host's fires are the shared ones), and a client may only ask
// it silenced; the beacon sets its
// `isActive` through `open`, which also lights its radar point. No lane carried the switch, so each peer's press was
// its own: a client's lamp lit on its machine alone, and a client's hot plate cooked nothing on the host. Here the
// host watches those bodies' ends: each change of the switch goes to every client (PropSwitchState), and a joiner gets
// every switch at its ready edge. A client's press runs on its copy as it is pressed, and the change is asked of the
// host (PropSwitchIntent), which judges the sender's reach and switches its own copy, whose state answers every peer;
// a refused ask is answered with the host's state, which puts the asker's copy back. A client takes a state through
// the prop's own `upd` or `open`, and holds the newest state of a prop whose copy is not bound yet until it is.
// Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropSwitchPayload;
}  // namespace coop::net

namespace coop::prop_switch_sync {

// Keep the session and register the watches on each switched class's `upd` or `open`. Idempotent.
void Install(coop::net::Session* session);

// Settles the watches and logs whether they are live. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every switched prop's state, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's ask, its length checked by the dispatcher.
void OnIntent(int senderSlot, const coop::net::PropSwitchPayload& p);

// CLIENT: the host's state, its length checked by the dispatcher.
void OnState(const coop::net::PropSwitchPayload& p);

// CLIENT (coop/props/remote_prop): a prop copy was bound to element id `eid`; a state held for it is applied.
void OnBound(uint32_t eid, void* actor);

// CLIENT: the world changed under the session: the states held and the switches seen in the old one go.
void OnClientWorldReady();

// Session end: the records are cleared.
void OnDisconnect();

}  // namespace coop::prop_switch_sync
