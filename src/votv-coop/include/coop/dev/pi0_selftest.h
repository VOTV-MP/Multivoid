// coop/dev/pi0_selftest.h -- [dev] proves the Player Element keeps its id across a cleared puppet.
// The registry half of PI-0 that the rig cannot force: a spawn whose finish call fails calls
// players::Registry::ClearPuppet, and the retry's RegisterPuppet must bind its puppet to the Element
// the slot already holds instead of dropping it and minting a second id for the same peer.
//
// Once the host's own Player Element exists (the role is config, so no peer need be connected) it
// takes the highest peer slot, if nobody holds it, and runs
// RegisterPuppet, ClearPuppet, RegisterPuppet on it with a RemotePlayer that is never spawned (the
// registry reads the puppet's actor pointer and nothing else of it, and an unspawned one answers
// null). It prints one [PI-0-SELFTEST] line, PASS when the Element's id is unchanged and the second
// puppet is bound, FAIL when it is not, ABANDONED when the slot is in use, then UnregisterPuppet
// leaves the registry as it was found. Host only; armed per run by pi0_selftest=1.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::pi0_selftest {

// The pump tick's entry: one latched flag read when off. Runs the sequence once per process, on the
// first host tick on which its own Player Element exists. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::pi0_selftest
