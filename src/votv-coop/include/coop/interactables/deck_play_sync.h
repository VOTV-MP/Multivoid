// coop/deck_play_sync.h -- L6: unit-3 deck PLAYBACK sync (PlayDeckEvent=107,
// v117). Design: 7-round /qf 2026-07-18 (converged "that holds" R7); fact
// base = the whole-asset bytecode census in the design finding.
//
// SHAPE -- presser-authored edge events at the v115 audio Func seam:
//   PLAY:  the ONLY signalSound.Activate site in the desk BP is playSignal's
//          body (census x1, operand bReset=TRUE) -> an organic (non-wire)
//          Activate on the desk's signalSound IS "a peer started playback"
//          -> broadcast {play, selectIndex, gen}.
//   STOP:  the ONLY Deactivate site is stopSound's body (census x1) -> an
//          organic Deactivate IS "playback stopped" -- covers the stop
//          button (any peer -- the deck is claim-free world buttons), the
//          power-off side effect, and IMPORT/EXPORT (invariant, not a verb
//          list) -- EXCEPT inside the fin() PE bracket (natural track end:
//          every peer's own copy self-terminates; broadcasting it would be
//          N-peer spam).
//   GEN GUARD (correctness is INDEPENDENT of fin's inferred PE visibility):
//          the play author mints max(seenGen)+1; a stop carries the gen of
//          the playback it terminates; receivers drop stale-gen + duplicate
//          stops. If fin proves EX-dispatched (bracket never fires), every
//          natural-end stop is dropped by the guard -- no cross-kill of a
//          restarted (higher-gen) playback.
//   APPLY: pre-check active_play + index validity (rows are byte-identical
//          across peers -- the decoded gate cannot diverge; a failed check
//          is a WARN + one silent track, self-healing), route selectIndex
//          through the v112 DeskInput apply author (write + echo-prime --
//          closes the scroll-then-play race where the 250 ms poll's delta
//          does not exist yet), then reflected playSignal()/stopSound()
//          under the shared audio-seam wire guard.
//
// JOIN: no playback seed (a joiner misses in-flight playback -- accepted
// arch residual; volume/index/toggles seed via DeskState adopt). LEAVER:
// fin self-terminates on every peer natively. Dev self-test ([dev]
// deck_selftest=1): the host dispatches a reflected organic Activate ->
// Deactivate pair on signalSound -- proves patch/routing/classification/
// wire/gen pre-hands-on (the full playSignal audio positive is the take's).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::deck_play_sync {

void Install(coop::net::Session* session);

// Per-net-pump: lazy seam install (Activate chain + the Deactivate Func
// patch + the fin PE pre/post bracket), detour-ring flush -> wire, the dev
// self-test, and the 60 s seam-fire evidence counters.
void Tick();

// PlayDeckEvent from the wire (router: event_dispatch_signal.cpp).
void OnPlayDeck(const coop::net::PlayDeckEventPayload& p, uint8_t senderSlot);

// Full state reset (gen counters, ring, self-test latch). Wired into the
// subsystems.cpp teardown fanout -- every session-end path runs it.
void OnDisconnect();

}  // namespace coop::deck_play_sync
