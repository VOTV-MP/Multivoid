// coop/props/trash_morph_gate.h -- on a client, the game's own trash actors do not author their
// own morph.
//
// A chip pile turns itself into a clump through `toClump` -- called by its collision component's
// overlap handler and by the arir follower -- and through `playerGrabbed`, which spawns the
// carried clump inline; a clump re-piles itself on its first level contact. Each spawns the
// successor and destroys the actor the body runs on. On a client every trash transition belongs
// to the host and arrives as PropConvert, so a local one is a second author: it destroys the
// mirror the host is driving and leaves an actor no element id owns.
//
// The seam is the script-body gate, not a ProcessEvent interceptor: the game reaches
// `playerGrabbed` with an EX_LocalVirtualFunction through an interface context and `toClump` from
// a Blueprint graph, both of which the VM routes below the detour, so an interceptor would
// register cleanly and never fire. The gate sees every Blueprint body on every route and refuses
// one per call. The host is untouched and keeps authoring natively. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::trash_morph_gate {

// Cache the session the refusals read and watch the three verbs by class and name (the watches
// are registered once per process; the session is re-cached at every call, so a reconnect keeps
// the refusals live). From the session start, before the world: a watch needs no loaded class,
// since the gate matches by the owning class's name from the first instance. Called from the
// session start (the TimelineThread); the name resolve is driven by Tick.
void Install(coop::net::Session* session);

// The owner of the resolve: until the three watches are live (or dead for good) it asks the script gate to
// resolve the pending names, and logs "3/3 ... live" once. One bool check after. Game thread, every session
// tick.
void Tick();

// Zero the per-verb tallies so a second session's numbers are its own. Game thread.
void OnSessionStart();

// Drop the cached session: with no session the gate refuses nothing and single-player trash
// behaves exactly as the game wrote it. Game thread.
void OnDisconnect();

}  // namespace coop::trash_morph_gate
