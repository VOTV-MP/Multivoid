// coop/props/trash_proxy.h -- the host-authoritative trash mirror proxy.
// The client's mirror of a trash entity used to be a real chip-pile or garbage-clump
// blueprint actor. That blueprint runs its own graph, so it self-morphs, self-destructs and
// is GC-eligible on its own schedule, independent of the host; within seconds it went
// not-live, the eid resolve returned null, and the next convert spawned fresh while the
// original lingered: the visible duplicate. The mirror is instead a static mesh actor we
// own: no blueprint, so it never self-morphs; rooted, so it is never collected; and in our
// eid-to-actor map, so a convert re-skins it in place instead of spawning fresh. The proxy
// is a kinematic host-driven follower with no collision, so the client's grab aim is a
// camera-ray cone, not the game's interaction trace (a bare mesh actor can never be the
// game's look-at actor, and the worn pile mesh has no simple collision body). Scope: trash
// only; other prop and kerfur mirrors are unchanged. This module owns the eid-to-proxy
// registry, and the GC pin rides the registry entry by value, so erasing the entry is the
// un-root and no retire path can forget it or make it conditional. The membership-reconcile
// sweep skips mirrors, so it is not a proxy retire path. Game thread.

#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>
#include <string>

namespace ue_wrap { struct FVector; struct FRotator; }

namespace coop::trash_proxy {

// Does this wire class get the host-authoritative proxy treatment? True for the self-morphing
// trash-litter family: the chip-pile and garbage-clump classes and their variants (a
// descendant test against the two base classes, not a string match; cached per class name).
bool IsTrashProxyClass(const std::wstring& className);

// Is this wire class a garbage-clump variant (versus a chip pile)? Picks the proxy's initial
// form at spawn (a placed pile at join versus a clump expressed mid-carry). Game thread.
bool IsClumpClass(const std::wstring& className);

// Spawn a proxy mirror for trash entity `eid`: a static mesh actor wearing the chip-type pile
// mesh (or the clump mesh if `isClump`), rooted, explicit no-collision, at the given
// transform, tracked in the registry. If `eid` already has a live proxy (a re-seed,
// duplicate or snapshot-bootstrap spawn), returns that proxy untouched, never a second actor
// and no re-skin: the convert path owns the form. `scale` is the host's real actor scale for
// this form; a static mesh actor defaults to unit scale, so without it the proxy rendered
// smaller than the host's pile or clump. Returns the actor (the caller registers it as the
// prop mirror for the pose drive and the convert resolve), or null on failure. Game thread.
void* SpawnProxy(coop::element::ElementId eid, uint8_t chipType, bool isClump, int ownerSlot,
                 const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, const ue_wrap::FVector& scale);

// Re-skin proxy `eid` in place (pile to clump or back), the convert path: a mesh swap on the
// same actor, the eid-to-actor binding never touched. Also applies `scale`, since a clump
// and a pile differ in size. Returns the proxy actor, or null if `eid` is not a tracked
// proxy. Game thread.
void* ReskinProxy(coop::element::ElementId eid, uint8_t chipType, bool isClump, const ue_wrap::FVector& scale);

// Retire proxy `eid`: take the pin out of the entry, erase the entry, evict the drives,
// destroy, unbind the element, with the pin releasing at scope exit, after the destroy. The
// erase comes first so nothing below can re-enter and find a half-retired entry; the release
// comes last because destroy marks pending-kill and the un-root is what makes that memory
// reapable. The destroy is conditional on liveness; the release is not, which is the whole
// point of the pin owning itself. No-op if `eid` is not a tracked proxy. Game thread.
void RetireProxy(coop::element::ElementId eid);

// Retire only the proxy actor for `eid` (the same order as RetireProxy) without unbinding its
// prop mirror or element. The caller must already have rebound `eid` onto a replacement
// actor in place: this is the clump-proxy to native-pile hand-off, the inverse of the native
// to clump morph. RetireProxy here would delete the element the native now owns. No-op if
// `eid` is not a tracked proxy. Game thread.
void RetireProxyActorOnly(coop::element::ElementId eid);

// Is `eid` a tracked trash proxy? Lets the wire receiver branch to ReskinProxy or RetireProxy
// instead of the blueprint spawn path. Game thread.
bool IsProxy(coop::element::ElementId eid);

// Nearest pile-form proxy actor to `fromLoc` (skips clump-form proxies and dead ones). Null if
// none. `outDistCm` (optional) gets the distance, or -1 on a miss. Used by the pile-spawn
// bind's orphan census. Game thread.
void* NearestPileProxy(const ue_wrap::FVector& fromLoc, float* outDistCm);

// The live proxy actor for `eid`, or null if `eid` is not a tracked proxy or its actor is dead.
// The clump pose stream resolves its per-eid carry target through this, and the use
// intercept its carried proxy. Game thread.
void* ProxyActorForEid(coop::element::ElementId eid);

// Client-grab recognition: the eid of the pile-form proxy the local player is aiming at,
// within `maxRangeCm` of `camLoc` and most centred on the unit aim ray `camFwd` (largest
// cosine at or above `minDot`). The invalid id if none qualifies. A camera-ray cone, not an
// engine trace: the proxy carries no collision, so the use intercept aims by math, and the
// host re-validates the eid, which bounds a cone mis-pick. Game thread.
coop::element::ElementId EidForAimedPileProxy(const ue_wrap::FVector& camLoc, const ue_wrap::FVector& camFwd,
                                             float maxRangeCm, float minDot);

// Retire every proxy owned by `slot` (a single peer dropping while the session stays up).
// Must run before the generic per-slot mirror drain, which would otherwise drain a proxy's
// prop element while leaving the registry entry, and so its pin, in place: a rooted leak.
// Game thread.
void OnDisconnectForSlot(int slot);

// Retire every proxy (net disconnect): the structural no-leak backstop so a rooted proxy
// never survives the session. Game thread.
void OnDisconnect();

}  // namespace coop::trash_proxy
