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
// 7: coop/ network layer. The clump<->pile link is driven from the VISIBLE seams (the InpActEvt_use PRE
// records a pending grab; the held-object edge adopts the clump) -- NOT the BeginDeferred spawn POST,
// which is EX_CallMath-dispatched and never reaches our ProcessEvent hook (2026-06-21 RE/hands-on).
// GAME-THREAD only (every entry point runs on the net-pump game thread).

#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>

namespace coop::net { class Session; }
namespace ue_wrap { struct FVector; struct FRotator; }

namespace coop::trash_channel {

// HOST: a trash entity re-skinned (grab pile->clump, or land clump->pile).
//   E        = the host-minted eid of the entity being re-skinned (the grabbed pile / the re-piling clump).
//   kind     = propconvert_kind::kToClump (grab: pile->clump) | kToPile (land: clump->pile).
//   newActor = the new rendering (the clump on a grab / the new pile on a land), already positioned.
//   loc/rot  = newActor's transform; chipType = the trash variant (carried across both edges).
// Bumps E's ctx, re-skins E onto newActor locally, and broadcasts PropConvert{E,kind,ctx,loc,rot,
// chipType} to all peers. Host-only. Driven from AdoptPendingGrabClump (grab) / the re-pile watch (land),
// NOT a spawn POST -- the chipPile/clump spawns are EX_CallMath-dispatched -> invisible. Game thread.
void OnHostConvert(coop::net::Session& s, coop::element::ElementId E, uint8_t kind, void* newActor,
                   const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, uint8_t chipType);

// HOST: the local player released/threw the trash entity bound to E (its carry ended -> FLYING). Bumps
// ctx and returns the new value to stamp on the outgoing PropRelease. Returns 0 if E is not a tracked
// trash entity (a normal prop release -> no ctx enforcement). Game thread.
uint8_t OnHostRelease(coop::element::ElementId E);

// ---- GRAB ADOPTION (the VISIBLE-seam clump<->pile link, replacing the dead BeginDeferred POST) -------
// The chipPile's clump spawn is dispatched EX_CallMath (a native thunk) -> INVISIBLE to our ProcessEvent
// hook (votv-clump-lifecycle-observability ...-2026-06-08-pass2, confirmed by the 2026-06-21 hands-on).
// So the convert is NOT caught at the spawn; it is driven from the two VISIBLE seams instead: the
// InpActEvt_use PRE (the pile is aimed + alive -> its eid is known) records a PENDING grab, and the
// held-object edge (the clump is now in hand) ADOPTS that clump onto the pending pile's eid.

// HOST: the local player pressed E aiming at tracked pile `pileEid` (InpActEvt_use PRE). The clump the BP
// spawns next is this pile's grab product. chipType is captured here (the pile is alive at PRE; the new
// clump's own chipType isn't written yet). A later press overwrites an unconsumed pending grab. Game thread.
void NotePendingGrab(coop::element::ElementId pileEid, uint8_t chipType);

// HOST: a garbage clump just entered the hand (the held-object new-held edge). If a recent NotePendingGrab
// is pending, ADOPT `heldClump` onto that pile's eid E: OnHostConvert(E, ToClump, heldClump, loc, rot,
// chipType) -- bumps ctx, rebinds E onto the clump, broadcasts PropConvert{ToClump} -- and returns E (the
// caller sets g_lastHeldEid=E so the carry stream stamps ctx). Returns kInvalidId if there is no pending
// grab (a normal held prop -> not a trash convert). Caller gates on IsGarbageClump. Game thread.
coop::element::ElementId AdoptPendingGrabClump(coop::net::Session& s, void* heldClump,
                                               const ue_wrap::FVector& clumpLoc,
                                               const ue_wrap::FRotator& clumpRot);

// HOST: decay the pending-grab TTL once per gameplay tick (clears a grab that produced no clump, so a
// later unrelated clump pickup can't be mis-adopted). Game thread.
void TickPendingGrab();

// The current per-eid sync-time-context (for local_streams to stamp on each carry PropPose). 0 =
// untracked / non-trash (no ctx enforcement). Game thread.
uint8_t CtxForEid(coop::element::ElementId E);

// RECEIVER: a PropConvert for E arrived with sync-time-context `ctx`. Returns true if FRESH (apply +
// adopt the host's ctx as our new known generation); false if STALE (an out-of-order / duplicate
// convert -> drop). ctx==0 = legacy/non-trash (always fresh, no adoption). Game thread.
bool AdoptInboundConvertCtx(coop::element::ElementId E, uint8_t ctx);

// RECEIVER: a PropPose / PropRelease for E arrived with `ctx`. `requireCurrentGen` picks the gate by packet
// kind: a CARRY POSE passes TRUE (apply only the CURRENT generation, ctx == known -- HOLD a pose ahead of
// its convert, the 2026-06-21 triple-grab-sound + pile-jump fix; DROP a stale one); a RELEASE passes FALSE
// (apply if NOT stale, ctx >= known -- a throw legitimately leads the last convert since it is not a
// re-skin). ctx==0, or an E we've seen no convert for, = no/hold enforcement. Game thread.
bool IsInboundStreamCtxFresh(coop::element::ElementId E, uint8_t ctx, bool requireCurrentGen);

// Drop all per-eid state (net disconnect).
void OnDisconnect();

}  // namespace coop::trash_channel
