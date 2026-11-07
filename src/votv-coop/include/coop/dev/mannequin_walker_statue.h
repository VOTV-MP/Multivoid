// coop/dev/mannequin_walker_statue.h -- [dev] the walker drill's third increment: step 8, the statue route the prop lane carries.
//
// The mannequin ticker has a second route besides the walker's spawn: it picks a statue (prop_mannequin_C) from the gamemode's list,
// spawns a plain prop_C named mann_b (the broken mannequin) at its transform, destroys the statue, and then spawns a walker at a spawn
// point. This client refuses the ticker's body (spawn_authority), so what it shows of the route is what the prop lane carries: the
// swap and the stand's end as the destroy and spawn of keyed props. This step runs it on the host and judges, on the client, that
//   - the client held a copy of the stand the ticker swapped, before the swap (it was snapshotted before the host's tick);
//   - after the swap that copy is gone;
//   - the broken mannequin prop (a prop_C, by its key) has a copy;
//   - the walker the ticker spawned has a copy, by its key.
// The route is the game's own: the host writes the day number the ticker's gate reads and the roll's weight, runs the ticker's tick,
// and writes both back (no player moves and no actor is placed by hand). A world with no statue or no ticker is an INVALID naming
// that; a route that gives no walker, or no broken prop, within its bound is an INVALID too (the game's own refusal, not a lane's
// failure). The peers (marks): host statue_go ... client statue_snap; host statue_swap ... client statue_done ("ok", or "fail step 8
// ..." which the host turns into the verdict).

#pragma once

namespace coop::dev::mannequin_walker_drill::statue {

// The host's half, after step 7: says the verdict itself on a failure or an INVALID and returns false then. Called from the drill's own thread
// (it hops onto the game thread itself).
bool Host();

// The client's half, in the rejoined life after it reported step 7: false when a check failed or a mark did not come (the verdict is
// the host's).
bool Client();

}  // namespace coop::dev::mannequin_walker_drill::statue
