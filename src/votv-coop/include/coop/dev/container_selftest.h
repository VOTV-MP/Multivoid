// coop/dev/container_selftest.h -- the R11b e2e instrument for the bidirectional container
// contents lane (`[dev] container_selftest=1`).
//
// WHY IT EXISTS. An idle join smoke cannot exercise this lane at all: nobody opens a container,
// and a save load fills `saveSlot.GObjStack` wholesale rather than through `addObject`. So the
// lane's single most important claim -- that the 0x45 `addObject`/`takeObj` callback actually
// ENTERS on each peer -- is invisible to the smoke, exactly the gap
// [[feedback-interaction-smoke-not-join-smoke]] names. R11 already cost TWO RED hands-on takes to
// a callback that never ran while every banner reported success
// ([[lesson-late-registrant-inert-after-all-resolved-latch]]); this instrument is what keeps a
// third one from being spent on the same question.
//
// HOW IT STAYS ORGANIC. It dispatches `propInventory_C::addLoot()` -- a parameterless verb -- and
// nothing else. `addLoot` calls `addObject` FOUR times through `EX_LocalVirtualFunction` (bytecode
// census, votv-container-contents-gobjstack-RE-2026-07-22 §5), so the call WE make is the outer
// one and the mutation that must be caught is the game's own inner dispatch. Dispatching
// `addObject` directly would prove nothing -- it would arrive through ProcessEvent, i.e. the very
// path the lane does NOT rely on ([[feedback-probe-must-count-not-confirm]]: never resolve through
// the mechanism under test).
//
// WHAT IT PROVES, in this order:
//   +10s HOST   addLoot on a world container -> host verb ENTERS -> host authors -> client applies
//   +25s CLIENT addLoot on a different one   -> client verb ENTERS -> client authors -> host
//                                               arbitrates (baseHash CAS) -> applies -> relays
// Each peer prints a DIGEST line (eid, record count, currVol) for both containers every 5s, so the
// smoke can compare the NUMBER across peers -- resolving a re-derive UFunction is not the same as
// the volume converging, and only the number settles it.
//
// This is a dev instrument (RULE 2 does not retire probes,
// [[feedback-rule2-exempts-probes-diagnostics-tools]]). It never runs with the flag off.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_selftest {

// Cache the session pointer. Call once at boot (subsystems Install). No-op with the flag off.
void Install(coop::net::Session* session);

// Game thread, per tick. Drives the two scheduled dispatches and the 5s digest. No-op with the
// flag off, before the session connects, or once both dispatches have fired.
void Tick();

// Clear the schedule + the resolved containers so a reconnect re-runs the circle.
void OnDisconnect();

}  // namespace coop::dev::container_selftest
