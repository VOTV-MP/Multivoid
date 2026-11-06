// coop/creatures/walker_state_host.h -- the host's walking mannequin on the wire: its pose and the player it hunts.
//
// A walker's copy on a client runs no brain (coop/creatures/walker_brain), so what the walker's own Blueprint decides crosses as
// a WalkerState {key, looking, target}, sent on each edge of either field and to a joiner alone at its ready edge. `looking` is
// the walker's own flag: its Blueprint writes `looking` in one place, its tick, and each time that write changes it the same
// chain writes the mesh's angular and linear damping and the anim's `stand`. The damping write is the engine's own announcement
// of the edge, a native the Blueprint calls, so this is a post hook on PrimitiveComponent.SetAngularDamping and no poll: it
// reads `looking` after the write and sends. `target` is the hunt's (coop/creatures/walker_target), taken from its change
// subscription.
//
// THE HOOK. SetAngularDamping is called by every prop that has a body, so the hook's first act is one compare of the calling
// frame's function with the walker's graph, which every other caller fails; the walker's own calls are then named by the offsets
// they end at, found in the graph's bytecode (coop/creatures/walker_graph). It is armed only while a walker is held (the
// target module's graph subscription), disarmed otherwise. Host only; the hook runs on the game thread inside the engine's call.
// The walker's anger does not cross (the walker's `looking` is false while it is angry, and crosses as that). Coop layer.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::walker_state_host {

// The host's subscriptions, once for the process, from the session's first tick (coop/session/subsystems,
// InstallLoadWatchers). A client pays nothing. Game thread.
void Install(coop::net::Session& session);

// Every held walker's state to the joiner at `slot` alone, at its ready edge (the connect replay). A walker whose copy is not
// there yet is held by the client until it is. Game thread.
void SendJoinStateToSlot(int slot);

// The session ended: the session pointer and the pinned sites are forgotten, the hook is disarmed. Game thread.
void OnDisconnect();

}  // namespace coop::walker_state_host
