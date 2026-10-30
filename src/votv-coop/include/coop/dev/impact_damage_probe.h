// coop/dev/impact_damage_probe.h -- [dev] what comp_physicsImpact_C::impactDamage receives, on both roles. Read-only.
//
// A script-gate watch on the component's impactDamage body (observe only, never a cancel). Each call prints one line
// with the damage, the actor argument and whether it is this machine's own player, a remote player's puppet, or a prop
// this machine authors; the owner prop; and who called it. The calls of a player's swing, a prop's physics impact and a
// drill's reflected call differ in exactly those fields, which is what a rule that judges the actor needs to know.
// Ini impact_damage_probe=1 or VOTVCOOP_IMPACT_DAMAGE_PROBE=1. Off, no watch is registered. Game thread.
#pragma once

namespace coop::dev::impact_damage_probe {

// Registers the watch while the row is on, retried each second until it is live; a latched read when off.
// Re-entered every pump tick.
void Install();

// Session end: the count of calls past the line cap is said and the totals cleared.
void OnDisconnect();

}  // namespace coop::dev::impact_damage_probe
