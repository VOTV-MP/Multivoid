// coop/trash_channel.h -- the HOST-AUTHORITATIVE trash-entity sync-time-context (docs/piles/08).
//
// The pile-sync redesign's identity + freshness core. A trash entity is a host-minted eid that
// re-skins in place across pile -> clump -> pile (oldEid == newEid == E); POSITION IS NEVER IDENTITY,
// so a dense pile CLUSTER can never mis-bind (the v81 morph's proximity FindNearestChipPile false-fired
// on a neighbour -> the dupe; docs/piles/08 "Why 07 failed"). This module owns the per-eid MTA
// sync-time-context (`ctx`): the HOST bumps it on EVERY transition (grab / throw / land) and stamps it
// on every convert/carry/throw packet; RECEIVERS drop any packet whose ctx is older than the eid's
// known generation -- so a carry/land packet still in flight when the entity transitions can never be
// re-applied to the re-skinned entity (the one guard the morph lacked).
//
// MTA precedent: CElement::GenerateSyncTimeContext / CanUpdateSync (reference/mtasa-blue,
// CElement.cpp:1281/1300). Increment 1 (host-grab direction) is ctx-only; the PILED/HELD/FLYING state
// machine + the client GrabIntent/ThrowIntent handlers land with the client direction (v83). Principle
// 7: coop/ network layer; the convert spawn is caught by host_spawn_watcher (ue_wrap seam) and broadcast
// here. GAME-THREAD only (every entry point runs on the net-pump game thread).

#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>

namespace coop::net { class Session; }
namespace ue_wrap { struct FVector; struct FRotator; }

namespace coop::trash_channel {

// HOST: a chipPile/clump convert spawn was caught at the host_spawn_watcher convert-spawn POST.
//   E        = the host-minted eid of the SOURCE actor (WorldContextObject -- the dying pile on a grab,
//              the dying clump on a land).
//   kind     = propconvert_kind::kToClump (grab: pile->clump) | kToPile (land: clump->pile).
//   newActor = the just-spawned actor (BeginDeferred ReturnValue). NOT YET POSITIONED -- loc/rot come
//              from the SpawnTransform PARAM (the spawn watcher reads it; GetActorLocation is origin
//              here), and chipType comes from the SOURCE (the clump's own chipType isn't written until
//              after BeginDeferred returns -- bytecode @2790).
// Bumps E's ctx, re-skins E onto newActor locally, and broadcasts PropConvert{E,kind,ctx,loc,rot,
// chipType} to all peers. Host-only (the spawn watcher is host-gated). Game thread.
void OnHostConvert(coop::net::Session& s, coop::element::ElementId E, uint8_t kind, void* newActor,
                   const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, uint8_t chipType);

// HOST: the local player released/threw the trash entity bound to E (its carry ended -> FLYING). Bumps
// ctx and returns the new value to stamp on the outgoing PropRelease. Returns 0 if E is not a tracked
// trash entity (a normal prop release -> no ctx enforcement). Game thread.
uint8_t OnHostRelease(coop::element::ElementId E);

// The current per-eid sync-time-context (for local_streams to stamp on each carry PropPose). 0 =
// untracked / non-trash (no ctx enforcement). Game thread.
uint8_t CtxForEid(coop::element::ElementId E);

// RECEIVER: a PropConvert for E arrived with sync-time-context `ctx`. Returns true if FRESH (apply +
// adopt the host's ctx as our new known generation); false if STALE (an out-of-order / duplicate
// convert -> drop). ctx==0 = legacy/non-trash (always fresh, no adoption). Game thread.
bool AdoptInboundConvertCtx(coop::element::ElementId E, uint8_t ctx);

// RECEIVER: a PropPose / PropRelease for E arrived with `ctx`. Returns true if fresh-or-current
// (apply); false if STALE relative to E's known generation (drop -- a carry/throw packet from before
// the latest transition). ctx==0, or an E we've seen no convert for, = no enforcement (true). Game thread.
bool IsInboundStreamCtxFresh(coop::element::ElementId E, uint8_t ctx);

// Drop all per-eid state (net disconnect).
void OnDisconnect();

}  // namespace coop::trash_channel
