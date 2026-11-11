// coop/interactables/atv_damage_gate.h -- on a copy that does not own an ATV's tick, no route runs the ATV's damage
// or death. Three script-gate watches on ATV_C's own bodies cancel `addDamage` and `exploded` (the two verbs, which
// feed `impulse`, whose death branch calls `explode`) and the private `explode` itself (the story event's trap calls
// it with no verb) whenever coop::atv_sync::NonOwnerCopy names the object. The copy that owns the tick is the one
// authority for the ATV's damage; the others receive its health in the ATV lane's condition block. A subclass that
// inherits the bodies (car1_witch_C) is matched too.
//
// The watches fail open: a dead one leaves that route of an ATV's damage running on every copy, said in one ERROR
// line per row. (The collision guard fails closed, since without it the lane cannot run at all.)
// MTA also gates damage per copy: a client vehicle is damageable only while its driver is the local player or it
// syncs the unoccupied vehicle (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientVehicle.cpp:1210-1228,
// reference/mtasa-blue/Client/mods/deathmatch/logic/CDeathmatchVehicle.cpp:37-40), and the server takes health from
// the syncer's report (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:285, :330-351).
// The divergence is the mechanism: ATV_C has no can-be-damaged flag, so the gate cancels each route's call.

#pragma once

namespace coop::atv_damage_gate {

inline constexpr int kRows = 3;  // addDamage, exploded, explode

// Register the three watches. Called every pump tick: a refused registration is retried once a second, 30 times, then
// the row is dead and said once; once every row is registered the gate's names are resolved and the outcome said
// once. Latched after either; later calls return at once. Game thread.
void Install();

// Per row (addDamage, exploded, explode): the calls this copy cancelled and the ones it let run.
struct Counters {
    unsigned long long cancelled[kRows] = {};
    unsigned long long ran[kRows] = {};
};
Counters ReadCounters();

// How many calls of `row` this copy let run on `actor` itself (the totals above add every copy's).
unsigned long long RanOnActor(void* actor, int row);

// Zero the counters; the watches stay registered for the process.
void Reset();

}  // namespace coop::atv_damage_gate
