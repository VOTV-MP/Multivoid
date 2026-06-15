// coop/sleep_sync.h -- v71: the Minecraft-style SLEEP GATE.
//
// RE ground truth (votv-sleep-nightmare-RE-2026-06-12.md): SP sleep is ONE
// engine call (SetGlobalTimeDilation(20)) + ONE per-process world flag
// (mainGamemode.isSleep) + a cosmetic pawn/camera swap; the sleep need
// refills at the dilated rate read off the LOCAL flag, and the natural wake
// fires at need >= 100. All of it is per-process -- a lone coop sleeper would
// run its whole local world at 20x while the shared (host-authored) clock
// stands still, and awake peers near a shared flag would vital-drain at 20x.
//
// The gate (user decision 2026-06-12, the Minecraft shape):
//   WAITING -- a peer entering a bed keeps the native cosmetic half but this
//     module's per-tick enforcement undoes the dilation to 1.0 until everyone
//     sleeps. (Deliberate divergence from the RE doc's sleep()-detour
//     recommendation: polling the isSleep edge covers EVERY entry path --
//     interaction, dev probes, a host already asleep when a client connects
//     -- with no new hook surface, at a <=1-tick undo lag.)
//   TALLY -- each peer's isSleep edge reports inBed (SleepState op=Report);
//     the host counts world-ready peers and broadcasts "N/M sleeping" (chat
//     feed line).
//   ACCELERATE -- all in bed: host broadcast; every sleeping peer sets its
//     own dilation 20 (vitals refill natively per peer), the client clock
//     free-runs at TimeScale=1 for the phase (time_sync::SetSleepAccelerate).
//   END -- ANY peer's isSleep falling edge (natural host wake, manual exit,
//     hunger, event, nightmare -- all funnel through gamemode.wakeup) ends
//     the night for everyone: host broadcasts End{natural}; receivers still
//     in bed run the native wakeup() reflected. Only a NATURAL end (the host
//     slept to >= 100) grants every peer sleep=100 -- and strictly wakeup()
//     FIRST, need-write AFTER (wakeup rolls the 10% gearer gift iff the need
//     is >= 99 at call time; write-first would gift every mirror nightly).
//
// Authority details: CLIENTS clamp their need at 99 during the phase so only
// the HOST ends the night naturally (one authority; no first-to-fill race),
// and run dreamProbability=0 for the whole session (nightmares HOST-ONLY by
// design); the host's own roll is restored to the -1 sentinel only DURING
// the accelerate phase. A host nightmare wakes the house structurally
// (createDream wakeup()s before the dream -- the falling edge IS the End).
//
// MTA precedent: server-authoritative game speed (CGame::SetGameSpeed) +
// server-owned clock (CClock) + the host-tally-and-broadcast ready gate.
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::sleep_sync {

void Install(coop::net::Session* session);

// Per-tick: resolve (throttled), poll the local isSleep edge -> report,
// enforce the WAITING dilation undo, clamp the client need during the
// phase, manage the dreamProbability policy. Cheap when idle (a couple of
// cached field reads).
void Tick();

// Wire ingest (both roles; see SleepStatePayload op semantics).
void OnReliable(const coop::net::SleepStatePayload& p, uint8_t senderSlot);

// HOST: a joiner arrived (world-ready) -- ends a running accelerate phase
// (someone is now awake in the world) and re-tallies.
void QueueConnectBroadcastForSlot(int peerSlot);

// HOST: a leaver's inBed flag drops from the tally (their claim on the gate
// must not block the remaining sleepers).
void OnDisconnectForSlot(int slot);

void OnDisconnect();

}  // namespace coop::sleep_sync
