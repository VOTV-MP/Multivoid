// coop/trash_proxy.h -- the host-authoritative trash MIRROR proxy (phase 1).
//
// WHY: the client's mirror of a trash entity used to be a REAL actorChipPile_C /
// prop_garbageClump_C blueprint actor. That BP runs its own ubergraph -> it
// self-morphs / self-destructs / is GC-eligible (unrooted) on its OWN schedule,
// independent of the host. Within ~10 s it goes NOT-LIVE -> ResolveLiveActorByEid
// returns null -> the next OnConvert "spawns fresh" while the original lingers =
// the visible DUP (research/findings/votv-pile-mirror-staleness-robustness-DESIGN-
// 2026-06-21.md). The fix changes the mirror's RULES OF EXISTENCE: replace it with
// an AStaticMeshActor WE own --
//   - NO blueprint        -> never self-morphs / self-destructs
//   - AddToRoot           -> never GC'd -> never stale-index
//   - our eid->actor map  -> we own its lifetime; re-skin (SetStaticMesh) in place
//                            on convert, never spawn-fresh
// -> the dup is impossible BY CONSTRUCTION (the 3-verdict discriminator / health
// poll / serial-check are all moot).
//
// PHASE 1 = visual + position + re-skin, EXPLICIT NoCollision. The proxy is a
// kinematic host-driven follower (the host owns physics / grab / state); the
// client player TEMPORARILY passes through mirrored trash (a known, accepted
// phase-1 regression). Collision -- the garbageCollider hull, double-duty
// Pawn-block + grab-trace -- is PHASE 2, with the client-grab direction
// (Increment 2). Scope: trash only (chipPile / clump + variants); Aprop_C and
// kerfur mirrors are unchanged.
//
// LIFECYCLE: this module OWNS the eid->proxy registry + the rooting. Every retire
// path funnels through RetireProxy (Destroy -> RemoveFromRoot -> unbind, in that
// order -- GC can't interleave on the game thread). OnDisconnect sweeps the whole
// registry as the structural no-leak backstop (a rooted actor must never survive
// the session). The membership-reconcile sweep EXCLUDES mirrors (RunDivergenceSweep_
// `if (pr.mirror) continue;`), so it is not a proxy retire path. GAME-THREAD only.

#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>
#include <string>

namespace ue_wrap { struct FVector; struct FRotator; }

namespace coop::trash_proxy {

// Does this wire class get the host-authoritative proxy treatment? True for the
// self-morphing trash-litter family: actorChipPile_C / prop_garbageClump_C and
// their variants (an IsA test against the two base classes -- NOT a string match).
bool IsTrashProxyClass(const std::wstring& className);

// Is this wire class a garbageClump variant (vs a chipPile)? Picks the proxy's
// INITIAL form at SpawnProxy (a placed pile at join vs a clump expressed mid-carry).
// IsA test against prop_garbageClump_C. Game thread.
bool IsClumpClass(const std::wstring& className);

// Spawn a proxy mirror for trash entity `eid`: an AStaticMeshActor (NO blueprint)
// wearing the chipType pile mesh (or the dirtball clump mesh if isClump),
// AddToRoot'd, EXPLICIT NoCollision, at the given transform. Tracked in the module
// registry. If `eid` already has a live proxy (re-spawn convergence), re-skins it
// instead of leaking a second actor. Returns the actor (the caller
// RegisterPropMirror's it for the pose drive + OnConvert resolve), or nullptr on
// failure. Game thread.
// `scale` is the host's real GetActorScale3D for this form (v83): an AStaticMeshActor
// defaults to unit scale, so without it the proxy rendered SMALLER than the host's pile/clump.
// Applied on the FRESH spawn only (a convergence re-spawn keeps the convert-owned form+scale).
void* SpawnProxy(coop::element::ElementId eid, uint8_t chipType, bool isClump, int ownerSlot,
                 const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, const ue_wrap::FVector& scale);

// Re-skin proxy `eid` in place (pile<->clump) -- the dup-killing convert path:
// SetStaticMesh on the SAME actor; the eid->actor binding is NEVER touched (no
// spawn-fresh, no orphan). Also applies `scale` (v83): a clump and a pile differ
// in size, so the per-form host scale must be re-applied on every convert. Returns
// the proxy actor, or nullptr if `eid` is not a tracked proxy. Game thread.
void* ReskinProxy(coop::element::ElementId eid, uint8_t chipType, bool isClump, const ue_wrap::FVector& scale);

// Retire proxy `eid`: Destroy -> RemoveFromRoot -> unbind the Prop mirror. The
// single teardown helper (order per the GC-window analysis: destroy marks
// PendingKill, then un-root makes it GC-reapable -- a rooted PendingKill actor
// would leak). No-op if `eid` is not a tracked proxy. Game thread.
void RetireProxy(coop::element::ElementId eid);

// Is `eid` a tracked trash proxy? Lets the wire receiver branch to ReskinProxy /
// RetireProxy instead of the BP spawn-fresh path. Game thread.
bool IsProxy(coop::element::ElementId eid);

// Nearest PILE-form proxy actor to `fromLoc` (skips clump-form proxies + dead ones).
// nullptr if none. `outDistCm` (optional) gets the distance, or -1 on miss. Used by the
// visual-verification showcase (aim the client camera at a mirrored pile). Game thread.
void* NearestPileProxy(const ue_wrap::FVector& fromLoc, float* outDistCm);

// Retire every proxy owned by `slot` (a PER-SLOT disconnect -- a single peer
// dropping while the session stays up). MUST be called before the generic
// per-slot mirror drain (remote_prop::OnDisconnectForSlot -> DrainMirrorsForSlot),
// which would otherwise drain a proxy's Prop Element WITHOUT un-rooting its
// AStaticMeshActor = a rooted leak (CRITICAL-1). Game thread.
void OnDisconnectForSlot(int slot);

// Retire EVERY proxy (net disconnect): the structural no-leak backstop so a rooted
// proxy never survives the session. Game thread.
void OnDisconnect();

}  // namespace coop::trash_proxy
