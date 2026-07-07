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
// 7: coop/ network layer. The clump<->pile link is driven from the UFunction::Func thunk on
// BeginDeferredActorSpawnFromClass (v106: fires for EVERY dispatch route incl. EX_CallMath): the GRAB
// direction records a clump BIRTH CERTIFICATE (NoteClumpBorn) the held-object edge consumes; the LAND
// direction converts the re-piled clump in place (the 2026-06-21 deterministic re-pile).
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
// chipType} to all peers. Host-only. Driven from AdoptBornClump (grab) / the re-pile thunk (land).
// Game thread.
void OnHostConvert(coop::net::Session& s, coop::element::ElementId E, uint8_t kind, void* newActor,
                   const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, uint8_t chipType);

// (RULE 2, take-29 #2b: OnHostRelease RETIRED -- the trash throw no longer sends a PropRelease, so the
// throw-edge ctx bump is obsolete; the LAND COMMIT's BroadcastConvert already bumps the ctx.)

// ---- GRAB ADOPTION (v106: the DETERMINISTIC clump birth certificate) ---------------------------------
// Every garbage clump is born from a chipPile's BeginDeferred(clump) (bytecode: actorChipPile toClump @141
// + ubergraph @3493 -- srcObj IS the source pile, still alive at the POST). The UFunction::Func thunk
// (trash_collect_sync::OnBeginDeferredSpawnObserve) records the birth here: clump -> {source pile's eid,
// chipType}. This fires for EVERY grab route -- E-press AND use-HOLD (canBeUsedHold repeats with no new
// InpActEvt dispatch; the 2026-07-07 mis-bind root) -- so the held-edge consumes the certificate instead
// of guessing. (The old InpActEvt-PRE NotePendingGrab / AdoptPendingGrabClump pending-slot pair and the
// "a new clump while my carry is open = churn re-grab of MY last carry" heuristic are RETIRED, RULE 2:
// with 2+ clumps in flight the heuristic bound a foreign clump to the wrong lane -- eid 4809 stuck HELD
// forever, the client ghost-pile root.)

// HOST (thunk): clump `clump` was just spawned by the pile owning eid E; chipType read off the pile.
// Entries expire after a short TTL (a grab that never reaches the hand: hands full / denied). Game thread.
void NoteClumpBorn(void* clump, coop::element::ElementId E, uint8_t chipType);

// HOST (held-edge): consume the birth certificate for `clump`. Returns true + fills outE/outChipType
// if the thunk recorded it. Game thread.
bool TakeClumpBorn(void* clump, coop::element::ElementId* outE, uint8_t* outChipType);

// HOST: bind `heldClump` onto trash entity E + OPEN the carry: OnHostConvert(E, ToClump, ...) -- bumps
// ctx, rebinds E onto the clump, broadcasts PropConvert{ToClump}. Used by the held-edge for a certificate
// clump (fresh grab) and for a re-grabbed EXISTING tracked clump (a gate-aborted rest clump picked back
// up). Returns E. Game thread.
coop::element::ElementId AdoptBornClump(coop::net::Session& s, coop::element::ElementId E,
                                        void* heldClump, const ue_wrap::FVector& clumpLoc,
                                        const ue_wrap::FRotator& clumpRot, uint8_t chipType);

// ---- CLIENT-GRAB direction (Increment 2, v84, docs/piles/08) -- the host arm of the door OnRequest --
// A client sent GrabIntent{eid} (suppress-native + request, the interactable_channel.h:220 OnRequest shape;
// MTA CStaticFunctionDefinitions::AttachElements). The HOST validates + executes the real grab on puppet-N
// (probe-proven: research/findings/votv-puppet-grab-feasibility-RE-2026-06-22.md -- the grab ENGAGES on an
// unpossessed puppet, NO controller dependency) + broadcasts the authoritative PropConvert{kToClump}, then
// registers puppet_carry_drive (the puppet tick does NOT drive the PHC, so the host drives the hold pose).

// CLIENT: send a GrabIntent{eid} to the host (the client-grab REQUEST). The phase-2 OnPileGrabPre client arm
// calls this after suppressing the native grab; the synthetic harness test calls it to exercise the wire.
// No-op if `s` is not a running client. Game thread.
void SendGrabIntent(coop::net::Session& s, uint32_t eid);

// HOST: a client at peer `senderSlot` requested to grab trash entity `eid`. Gates (mirror the door OnRequest
// CanOpen + per-peer holdOpen_): eid not already carrying, eid tracked (has a ctx generation), the sender
// not already holding any eid. On pass: resolve puppet-N + the pile actor, run playerGrabbed(Player=puppet)
// via the probe-proven ParamFrame, read grabbing_actor synchronously, OnHostConvert(kToClump) (opens the
// carry latch + broadcasts), record HELD_BY(senderSlot), and NotePuppetHeld for the per-tick hand drive.
// No-op (logged DENIED) on any gate fail or a dead puppet/pile. Host-only. Game thread.
void OnGrabIntent(coop::net::Session& s, uint32_t eid, uint8_t senderSlot);

// CLIENT: send a ThrowIntent{eid,mode,dir} to the host. mode=throw_mode::kRelease (E-press toggle drop,
// dir ignored) | kHardThrow (LMB native throw, dir = the client's camera-forward unit vector at the press).
// No-op if `s` is not a running client. Game thread.
void SendThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode, const ue_wrap::FVector& dir);

// HOST: a client at peer `senderSlot` requested to throw the puppet-held trash entity `eid`. Gate: the
// sender must currently hold `eid` (HELD_BY). On pass: release the puppet's grab (clear grabbing_actor +
// PHC release -- REQUIRED so the clump's re-pile gate reads "not held"), enable physics + apply the throw
// velocity (mode=kRelease: puppet hand motion, capped; mode=kHardThrow: native cameraFwd*15000/max(mass,10)
// +puppetVel, NO cap, dir = client camera-forward), and NoteThrown so the host streams the flight not the
// hand. No-op (logged DENIED) on a gate fail / dead puppet / dead clump. Host-only. Game thread.
void OnThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode,
                   const ue_wrap::FVector& camFwd, uint8_t senderSlot);

// ---- CLIENT carry-state (the E-press grab/throw toggle, Increment 2 phase 2) -------------------------
// The client tracks the single trash eid its own puppet is carrying (a player holds one thing), so a
// client E-press is a THROW when carrying and a GRAB when aimed at a pile proxy. The state is driven by
// the convert stream the client already receives: SendGrabIntent marks a pending request; the matching
// inbound ToClump confirms the carry; the matching ToPile (or a throw send) clears it. CLIENT-side.

// CLIENT: the local player just observed an inbound PropConvert for `eid`. Reconcile the carry-state: a
// ToClump matching our pending grab CONFIRMS the carry; a ToPile for our carried eid CLEARS it. Called from
// remote_prop::OnConvert (client only). No-op on the host. Game thread.
void NoteClientConvertObserved(uint32_t eid, bool toClump);

// CLIENT: the trash eid the local player is currently carrying via its puppet, or kInvalidId if none. The
// OnPileGrabPre toggle reads this: carrying -> throw it; not carrying -> grab the aimed proxy. Game thread.
coop::element::ElementId ClientCarryEid();

// HOST: peer `senderSlot` disconnected -- clear any HELD_BY it owns (the eid becomes re-grabbable). The
// puppet vanishes on disconnect so the clump is already physics-released; this is the state cleanup +
// a ForgetEid so a stranded carry latch can't keep the entity in limbo. Game thread.
void OnGrabHolderLeft(uint8_t senderSlot);

// HOST: the puppet-held clump for `E` was LOST without a normal land (the clump died / the puppet went
// not-live -- puppet_carry_drive's non-land drop paths). Clear E's client hold (HELD_BY) + carry latch, AND
// broadcast PropDestroy(E) so EVERY client retires the frozen proxy + clears its carry-state toggle (else a
// client that loses a clump mid-carry is stuck in throw-mode forever, never grabbable again -- audit HIGH
// 2026-06-23). The trash entity genuinely vanished on the host (the clump died without re-piling), so a
// destroy is the honest authoritative edge. Idempotent. Game thread.
void ReleaseClientHold(coop::net::Session& s, coop::element::ElementId E);

// CLIENT: clear the local carry-state toggle (the player is no longer carrying anything). Called from the
// client PropDestroy path when the carried proxy is retired (the host aborted the carry) so the next
// E-press grabs instead of throwing a dead eid. No-op if not carrying that eid. Game thread.
void ClearClientCarry(uint32_t eid);

// ---- CLOSE-B carry latch + land-settle (host-side, 2026-06-22) ----------------------------------------
// A trash entity E is "carrying" from the real grab (OnHostConvert kToClump while not carrying) until the
// real land. DURING carry the host's stock churn -- the held clump re-piles on cluster contact ~1/s and the
// game auto-re-grabs (votv-chippile-carry-churn-...-2026-06-22) -- is SUPPRESSED: the re-pile (kToPile) is
// NOT broadcast and ctx is NOT bumped, so the client renders ONE clump (pose-streamed), not a pile stuck at
// the cluster re-skinned+teleported every cycle (the 2fps + non-disappearing-pile symptom, ONE root). The
// churn re-grab rebinds E onto the new clump (OnHostRegrab) so the carry stream stays alive. The REAL land
// is a re-pile NOT followed by a re-grab within kLandSettleTicks: the settle holds the ToPile broadcast that
// long; a re-grab CANCELS it (churn); the timeout COMMITS it (the one land) and closes the latch. Graceful:
// K too small -> a churn ToPile commits early, the re-grab re-opens -> a brief self-correcting
// clump->pile->clump flicker; K too large -> the land morph lags a few frames. Neither strands E. Per-eid.

// HOST: a garbage clump RE-ENTERED the hand during an active carry of E (the churn re-grab, observed at the
// held-object edge -- NOT a fresh player grab, so no pending grab). Rebind E onto `newClump` (the carry pose
// stream keeps tracking it) and CANCEL E's pending land-settle (a re-grab proves the preceding re-pile was
// churn, not the land). No broadcast, no ctx bump. No-op if E is not carrying. Game thread.
void OnHostRegrab(coop::element::ElementId E, void* newClump);

// HOST: is E mid-carry (the latch is OPEN)? local_streams gates the held-edge re-grab rebind on this. Game thread.
bool IsCarrying(coop::element::ElementId E);

// HOST: is a land-settle pending for E (a re-pile was observed, not yet committed/cancelled)? The release
// edge uses this to tell a CHURN flicker (holding_actor empties right after a re-pile -> settle pending) from
// a REAL drop/throw (an alive clump, NO pending settle -- a re-grab cancels the settle before any throw, so a
// real release never coincides with one). Suppress the release edge only when carrying AND this is true. Game thread.
bool HasPendingSettle(coop::element::ElementId E);

// HOST: the (single) currently-carried trash eid, or kInvalidId if none. There is at most one carried clump
// at a time (one held item). The simulateDrop thunk logs this to cross-check that a real drop/throw is the
// carried clump (vs an equip drop) before the flip wires the latch-close. Game thread.
coop::element::ElementId AnyCarryingEid();

// HOST: per-gameplay-tick pump --
//   1. prunes expired clump birth certificates;
//   2. counts down the land-settles, COMMITTING a settled land (broadcast the held ToPile + close the
//      latch) when no re-grab arrived within K;
//   3. GUARANTEED CARRY TERMINATION (v106): every open carry lane must eventually close. A lane whose
//      Registry actor DIED with no re-pile (consumed clump) closes + broadcasts PropDestroy(eid) after a
//      short grace; a lane whose clump lies AT REST un-held (the native re-pile gate aborted because the
//      thrower's hand was busy at land -- the 2026-07-07 eid-4809 permanent "already HELD" deny-lock)
//      closes silently: the clump stays world-tracked + re-grabbable, exactly the SP end state.
// `localHeldActor` = the local player's currently-held actor (rest-exclusion: a still player holding a
// clump must not read as "at rest un-held"). Game thread.
void TickCarry(coop::net::Session& s, void* localHeldActor);

// Drop E's carry latch + land-settle (E's entity was destroyed / retired, so the latch can never close on a
// land). Safety against a stranded latch; idempotent. Game thread.
void ForgetEid(coop::element::ElementId E);

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
