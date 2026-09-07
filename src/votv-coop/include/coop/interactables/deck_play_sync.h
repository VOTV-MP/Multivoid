// coop/interactables/deck_play_sync.h -- unit-3 deck PLAYBACK sync (PlayDeckEvent).
//
// Playback edges are authored by the presser at the desk audio Func seam, off two facts about the
// desk blueprint: the ONLY signalSound.Activate site is playSignal's body, with bReset=TRUE, and
// the ONLY Deactivate site is stopSound's body. So an organic -- non-wire -- Activate on the desk's
// signalSound IS "a peer started playback", and an organic Deactivate IS "playback stopped". Being
// an invariant rather than a verb list, the stop side covers the stop button on any peer, since the
// deck is claim-free world buttons, the power-off side effect, and import and export alike.
//
// The one exception is inside the fin() PE bracket, a natural track end: every peer's copy
// self-terminates there, so broadcasting would be N-peer spam.
//
// JOIN: no playback seed, so a joiner misses in-flight playback; volume, index and toggles seed via
// the DeskState adopt. LEAVER: fin self-terminates on every peer natively.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::deck_play_sync {

void Install(coop::net::Session* session);

// Per-net-pump: lazy seam install (the Activate chain, the Deactivate Func patch and the fin PE
// pre/post bracket), detour-ring flush to the wire, the dev self-test and the 60 s seam-fire
// evidence counters. The self-test ([dev] deck_selftest=1) has the host dispatch a reflected
// organic Activate/Deactivate pair on signalSound, which exercises the patch, routing,
// classification, wire and gen without a hands-on take.
void Tick();

// PlayDeckEvent from the wire (router: event_dispatch_signal.cpp).
//
// The play author mints max(seenGen)+1 and a stop carries the gen of the playback it terminates, so
// receivers drop a stale gen and a duplicate stop. Correctness does not depend on fin's dispatch
// visibility: if fin turns out to be EX-dispatched, so the bracket never fires, every natural-end
// stop is dropped by the guard and no restarted, higher-gen playback is cross-killed.
//
// Apply pre-checks active_play and index validity -- rows are byte-identical across peers, so the
// decoded gate cannot diverge, and a failed check is a WARN plus one silent track, self-healing --
// then routes selectIndex through the DeskInput apply author, whose write-plus-echo-prime closes
// the scroll-then-play race where the 250 ms poll's delta does not exist yet, and finally calls
// reflected playSignal() or stopSound() under the shared audio-seam wire guard.
void OnPlayDeck(const coop::net::PlayDeckEventPayload& p, uint8_t senderSlot);

// Full state reset (gen counters, ring, self-test latch). Wired into the
// subsystems.cpp teardown fanout -- every session-end path runs it.
void OnDisconnect();

}  // namespace coop::deck_play_sync
