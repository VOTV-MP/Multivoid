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

// Resolve the three verbs and watch their bodies (idempotent; the session pointer is re-cached
// on every call so a reconnect keeps the refusals live). Safe before the trash classes load: the class resolve is throttled and a
// partial set never latches, so a verb that is not resident yet is picked up later. Game thread.
void Install(coop::net::Session* session);

// Zero the per-verb tallies so a second session's numbers are its own. Game thread.
void OnSessionStart();

// Drop the cached session: with no session the gate refuses nothing and single-player trash
// behaves exactly as the game wrote it. Game thread.
void OnDisconnect();

}  // namespace coop::trash_morph_gate
