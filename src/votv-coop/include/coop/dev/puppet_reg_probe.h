// coop/dev/puppet_reg_probe.h -- [dev] proves a remote player's puppet is registered in players::Registry
// from inside its own spawn. A script-gate watch on mainPlayer_C's ReceiveBeginPlay records, at the
// instant the body begins play, whether Registry::IsPuppet already answers true for it; the pump tick
// binds each record to the peer slot whose puppet it is and judges it, once per puppet life, with one
// [PI-0] line that reads PASS or FAIL. The actor end-of-play seam judges the same
// record again at the puppet's end of play: it must still be registered then. A valid puppet whose
// begin-play was never observed reads ABANDONED. Both roles; read-only; armed per run by
// puppet_reg_probe=1.
#pragma once

namespace coop::dev::puppet_reg_probe {

// The pump tick's entry: one latched flag read when off. Arms its watch and its end-of-play sink on the
// first read that finds the row on, judges nothing until the watch has settled, then binds and judges
// the puppets of each slot. Game thread.
void Tick();

}  // namespace coop::dev::puppet_reg_probe
