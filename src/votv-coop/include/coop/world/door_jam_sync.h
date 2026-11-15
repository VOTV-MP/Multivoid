// coop/world/door_jam_sync.h -- which doors are jammed, the host's.
//
// `door_C::jam(true)` (run by trigger_jamDoor_C, the Reactor Restart, and by the Soltomia craft) sets the door's `jammed`, closes it and, with
// `autocancel`, unjams it 300 s later; a jammed door's use plays the jam shake instead of opening. The door lane carries open and closed and
// the shake's verbs but not the flag, so a copy was never jammed. HOST: once a second, each door whose `jammed` changed is sent on
// AmbientFixtureState (family kDoorJam): `id` a hash of the door's level name, `x y z` its place (a name is not unique across the level's
// tiles), `pitch` 1 jammed or 0 not. A joiner is sent every jammed door. CLIENT: the matching copy's `jammed` is written. Game thread.

#pragma once

namespace coop::net {
class Session;
struct AmbientFixtureStatePayload;
}  // namespace coop::net

namespace coop::door_jam_sync {

void Install(coop::net::Session* session);
void Tick();
void OnState(const coop::net::AmbientFixtureStatePayload& p);
void QueueConnectBroadcastForSlot(int slot);
void OnDisconnect();

}  // namespace coop::door_jam_sync
