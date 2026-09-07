// coop/sleep_sync.h -- the Minecraft-style SLEEP GATE: the night passes only once every peer is in
// bed. Game thread throughout.
//
// Single-player sleep is one engine call (SetGlobalTimeDilation(20)), one per-process world flag
// (mainGamemode.isSleep) and a pawn and camera swap that also drops whatever was held; the need
// refills at the dilated rate read off the LOCAL flag, and the natural wake fires at need >= 100.
// All of it is per-process, so a lone coop sleeper would run its whole local world at 20x while the
// shared, host-authored clock stood still, and awake peers near a shared flag would drain vitals at
// 20x. MTA's precedent for the answer: server-authoritative game speed (CGame::SetGameSpeed) and a
// server-owned clock (CClock), with a host tally-and-broadcast ready gate over them.
//
// Only a NATURAL end -- the host slept to need >= 100 -- grants every peer sleep=100, and strictly
// wakeup() FIRST, need-write AFTER: wakeup rolls a 10% gearer gift when the need is >= 99 and the
// bed is the plain bed_C, so writing the need first would gift every mirror nightly.

#pragma once

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::sleep_sync {

void Install(coop::net::Session* session);

// Per-tick: resolve (throttled), poll the local isSleep edge and report it, enforce the WAITING
// dilation undo, clamp the client need during the phase, and manage the dreamProbability policy.
// Cheap when idle -- a couple of cached field reads.
//
// The authority rules it enforces. CLIENTS clamp their need at 99 for the phase, so only the HOST
// can end the night naturally and a first-to-fill race becomes one authority; and they run
// dreamProbability=0 all session, nightmares being host-only by design. The host's own roll is
// restored to the -1 sentinel, which is the blueprint's "use the default", only DURING the
// accelerate phase. A host nightmare wakes the house structurally, because createDream calls
// wakeup() before the dream, so the falling edge IS the End.
void Tick();

// Wire ingest (both roles; see SleepStatePayload op semantics). The gate has four phases.
//
// WAITING -- a peer entering a bed keeps the native cosmetic half, but this module's per-tick
//   enforcement undoes the dilation to 1.0 until everyone sleeps. Polling the isSleep edge rather
//   than detouring sleep() covers EVERY entry path -- interaction, a probe, a host already asleep
//   when a client connects -- with no new hook surface and at most a tick of lag.
// TALLY -- each peer's isSleep edge reports inBed (op=Report); the host counts world-ready peers
// and
//   broadcasts "N/M sleeping" as a chat feed line.
// ACCELERATE -- everyone in bed: the host broadcasts, each sleeping peer sets its own dilation to
// 20
//   so vitals refill natively, and the client clock free-runs (time_sync::SetSleepAccelerate).
// END -- ANY peer's isSleep falling edge ends the night for everyone. Natural wake, manual exit,
//   hunger (food <= 20), an active event and a nightmare all funnel through gamemode.wakeup, so one
//   edge catches every case: the host broadcasts End{natural} and receivers reflect wakeup().
void OnReliable(const coop::net::SleepStatePayload& p, uint8_t senderSlot);

// HOST: a joiner arrived (world-ready) -- ends a running accelerate phase
// (someone is now awake in the world) and re-tallies.
void QueueConnectBroadcastForSlot(int peerSlot);

// HOST: a leaver's inBed flag drops from the tally (their claim on the gate
// must not block the remaining sleepers).
void OnDisconnectForSlot(int slot);

void OnDisconnect();

}  // namespace coop::sleep_sync
