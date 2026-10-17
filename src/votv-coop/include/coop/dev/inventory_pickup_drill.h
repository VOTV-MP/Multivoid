// coop/dev/inventory_pickup_drill.h -- dev-only: a CLIENT pockets loose props through the game's
// own pickup verb, once (env VOTVCOOP_INV_PICKUP_DRILL=1, off by default).
//
// The per-player profile is only proven by an item that is the client's ALONE: a joiner carrying
// nothing looks the same whether the profile works or the slot was merely emptied. The client
// pockets a plain item, a drive holding a signal and a disc holding data (a record's payload
// SHAPE survives a rejoin whether or not its VALUES do; with live_store_readout=1 the readout's
// `values` lines under one key in both lives are the comparison), sets food and sleep to numbers
// no fresh life has, and walks by the bot director to a NavMesh-reachable point, so the items,
// the vitals and the pose all have something to return. The pickup moves nobody:
// mainPlayer::putObjectInventory2 has no reach test. The host's half seeds such a drive and disc
// and, with VOTVCOOP_INV_PICKUP_DRILL_SAVE=1, saves when a joiner's world comes up, the only
// moment a host writes profiles to disk (`quick` alternates a quicksave with a plain save).
// Nothing waits on a clock: `CLIENT LIFE DONE`, `HOST SAVES DONE` and coop/session/rig_ready.h's
// lines are what a rig waits on. Both peers census the wanted classes' keys: one back is a dupe.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::inventory_pickup_drill {

// Fire once with the env set, on readiness: the host seeds once its player stands and its object
// index holds the world, then says so in the lobby chat; a client pockets once its join is over and
// that line is in its feed. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::inventory_pickup_drill
