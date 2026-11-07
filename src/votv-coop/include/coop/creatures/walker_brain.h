// coop/creatures/walker_brain.h -- a client's copy of the walking mannequin never thinks.
//
// The walking mannequin (prop_wMannequin_C) is a prop that carries a brain: its tick walks it and reads this machine's
// camera, its timers jump, hit and take up weapons, and its hunt re-enters on a pawn's move result. The host's walker is the
// one walker: it crosses as a prop, and the host's drive stream moves its copies. So on a client every entry of the walker's
// event graph is refused but two, by a script-gate watch on the graph, and nothing marks a copy: a client's spawner is
// refused (coop/world/spawn_authority), so a client holds only copies, and the gate is the role.
// Precedent: a creature mirror's brain off, npc_mirror_brain.h, which also cites MTA's remote ped. Coop layer; game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

// THE ENTRIES. 4411, the BeginPlay, runs: its parent call is the prop's own setup (the gamemode, the prop list, the
// thrown-state init) and its anim cast is the copy's presentation. What it starts is made harmless at its root: the nav pawn
// it spawns is made inert at its deferred spawn, so the BeginPlay's move gets a pawn with no controller and ends in the
// failure entry, which the gate refuses, and the four timers it arms fire into refused entries. 5249, the destroy, runs: it
// removes the copy from the gamemode's list and ends its pawn. Every other entry (the hunt, the tick, the timers, the move
// results, the fire) is refused.
// THE WINDOW. The deferred-spawn hook acts only on a spawn whose source is a walker, and only while this process holds a
// client session. A twin the client's own save loaded and the join bound is covered from its birth, as the join's load runs
// inside the session and the gate is installed with the session. The one window is a twin whose BeginPlay ran before the
// session began: the menu world's load is discarded by the join's, so no walker survives it.
namespace coop::walker_brain {

// A CLIENT session's own watch and hook, once for the process, from the session's first tick (coop/session/subsystems,
// InstallLoadWatchers): the graph watch (live once the game thread resolves its names) and the deferred-spawn hook, asked for
// again on a class-set move and then each tick for ue_wrap::class_reask::kLoadWindowMs while its row does not resolve (a class
// finishing its load moves no number); a row that resolved and whose hook the engine refused is asked again only on the next
// class-set move. Armed only while a client session runs. The host pays nothing. Game thread.
void Install(coop::net::Session& session);

// [dev], for the walker drill: the graph entries the gate has refused this session, all of them counted. Game thread.
uint32_t RefusedCalls();

// The session ended: the hook disarmed, what the gate refused said, the counters dropped. Game thread.
void OnDisconnect();

}  // namespace coop::walker_brain
