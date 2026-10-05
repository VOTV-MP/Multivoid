// coop/dev/damage_probe.h -- [dev] every call of the player's damage verb on this machine, one [DMG]
// line each: whose body takes it (this machine's own player, a peer's puppet by slot, or another
// actor), the amount, the source argument's class, and the Blueprint function and object that called
// it. It answers which route a hazard reaches a player by, and on which machine, before a relay is
// built on it. Armed by damage_probe=1; read-only, it never refuses a call.
#pragma once

namespace coop::dev::damage_probe {

// Registers the watch once, when armed; a latched flag read when off. Game thread.
void Tick();

}  // namespace coop::dev::damage_probe
