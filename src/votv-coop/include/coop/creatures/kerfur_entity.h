// coop/creatures/kerfur_entity.h -- the stable-identity layer for the kerfur. A kerfur is the
// only entity that is both a host-authoritative NPC (a character) and a grabbable prop,
// flipped by a player's turn-off and turn-on conversion, and the game gives it no stable
// cross-peer identity (its load mints a random key per peer per load, and the conversion
// verbs copy no id), so re-deriving identity from class and nearest pose on every conversion
// across two eid spaces produced the duplicate-and-drop loop. The fix, the MTA set-model
// shape: one host-allocated, host-range kerfur id per logical kerfur, spanning both forms.
// The radial-menu conversion is invisible to the dispatch detour, so the client's local
// conversion is unavoidable and claim-and-adopt stays, anchored to this stable id; clients
// never mint a kerfur eid. The registry model: a kerfur entity is a host-only authority
// element (its id reserved through the host allocator), held in a host-only table and not
// in the NPC or prop mirror managers; the rendered form is a normal NPC or prop mirror at its
// own per-form host-range eid in the existing managers, so the pose and physics pipelines
// are unchanged. The kerfur id is the durable handle; the per-form eids are the wire eids,
// and at any instant only the id and the current-form eid are live.

#pragma once

#include "coop/element/element.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::element {

// The host-side authority record reserving one stable host-range kerfur id per logical
// kerfur; not in the mirror managers (see the header). Construction registers nothing by
// itself: the host table allocates it to reserve the id, and the element destructor frees
// the id when the record is erased.
class KerfurEntity : public Element {
public:
    KerfurEntity() : Element(ElementType::Kerfur) {}
};

}  // namespace coop::element

namespace coop::kerfur_entity {

// 0 is the NPC form, 1 the prop form.
enum class Form : uint8_t { Npc = 0, Prop = 1 };

// Cache the session pointer, from the subsystem install fan-out (the same point as the
// conversion module). The id allocation reads the role through it; the form bind broadcasts
// through it.
void SetSession(coop::net::Session* session);

// Cache the resolved kerfur class pointers (the conversion module resolves them at its
// install and calls this once they bind), so the class tests answer via a subclass-aware
// walk without re-resolving. Idempotent on the same pointers.
void SetKerfurClasses(void* npcClass, void* propClass);

// True iff `cls` derives from the kerfur NPC or prop base. False until the classes are bound.
// Subclass-aware (the collar variants, the prop skins).
bool IsKerfurClass(void* cls);
// True iff `actor`'s class is a kerfur class. Null-safe.
bool IsKerfurActor(void* actor);
// True iff `cls` derives specifically from the kerfur prop base (and its skins), not the NPC
// base; the prop adoption collects prop-form candidates only (the NPC form is the NPC
// adoption's job). False until the prop base is bound.
bool IsKerfurPropClass(void* cls);

// Host only: reserve (or return the existing) stable kerfur id for a kerfur `actor` first
// seen in `form`, whose current-form wire eid is `currentEid`. Idempotent per actor. Records
// actor-to-id and eid-to-id. Returns the id, or invalid on a non-host, a null actor or
// registry exhaustion. Game thread.
coop::element::ElementId AllocKerfurId(void* actor, coop::element::ElementId currentEid,
                                       Form form, const std::wstring& className);

// Host only: the kerfur id behind a live wire eid, or invalid when the eid is not a tracked
// kerfur's current form. Constant time.
coop::element::ElementId GetKerfurIdForEid(coop::element::ElementId currentEid);

// Host only: the host eid of the off-prop that the kerfur currently at `currentEid`
// replaced; the NPC spawn builders read it to carry the off-to-active duplicate retire key
// to a joiner. Returns the off-prop eid iff this kerfur was off at the blob instant and the
// host turned it on in the join window (the form bind captured the origin eid); invalid for
// an always-active kerfur or a non-kerfur eid, and the builder then sends no retire. The
// joiner retires the off-prop mirror bound at this exact eid, no fuzzy position match. Game
// thread.
coop::element::ElementId GetOriginOffEidForEid(coop::element::ElementId currentEid);

// Host only: the host eid the kerfur currently at `currentEid` most recently converted from
// (the form bind's old eid). The mid-session turn-on NPC spawn builder carries it, so the
// initiating client adopts its parked conversion ghost by exact eid instead of a position
// match. Invalid for a never-converted kerfur, and the builder sends nothing. Game thread.
coop::element::ElementId GetConvertFromEidForEid(coop::element::ElementId currentEid);

// The client held-pose eid map. A kerfur prop on a client is a host-owned mirror at a
// host-range eid, not in the prop tracker's local map, so the local element lookup returns
// invalid for it; but a client carrying a kerfur prop must stream that mirror's host-range
// eid in its pose (the kerfur's blueprint key is random per peer, so only the eid identifies
// it cross-peer, and the host's receiver resolves the authoritative kerfur prop by that eid
// and drives it; no new packet). This client-only actor-to-eid map gives the local streams
// that eid in constant time. Populated by the mirror-bound notification from the prop mirror
// registration, the single chokepoint every kerfur prop mirror bind funnels through (the
// convert materialisation, the join snapshot, the fuzzy adopt); queried by the eid lookup,
// self-healing (a stale entry whose eid no longer binds the actor is evicted on read, so an
// address recycle cannot mis-resolve); cleared on disconnect. Game thread only; a no-op on
// the host, which owns the kerfur as a local and never mirrors it.

// Client: record that a kerfur prop mirror actor was bound at host-range `eid`. Self-filters:
// a no-op on the host or for a non-kerfur actor. Idempotent.
void NotifyKerfurPropMirrorBound(void* actor, coop::element::ElementId eid);

// Client: the host-range eid for a locally held kerfur prop mirror actor (for the held-pose
// stream), in constant time. Invalid if not tracked. Self-heals stale entries.
coop::element::ElementId GetKerfurMirrorEidForActor(void* actor);

// Client: evict an actor from the held-pose map (on a kerfur mirror teardown). A no-op if
// absent.
void ForgetKerfurPropMirror(void* actor);

// Host only: the conversion mutation, the MTA set-model equivalent. The host ran the
// blueprint verb (its own radial menu, or a client request) and registered the new-form
// actor silently via the normal NPC or prop pipeline at host-range `newEid`. This rebinds
// the stable kerfur id (resolved from `oldEid`, or freshly allocated if the dying form was
// untracked) onto the new form in place, the id preserved across the conversion and never
// re-minted, then broadcasts the sole conversion-transition packet (the id, the old and new
// eids, the form, the transform, the class) to all peers. Returns the id (invalid on a
// non-host, a null actor or registry exhaustion). The caller reads the new actor's
// transform and class and passes them, keeping this module's engine surface minimal. Game
// thread.
coop::element::ElementId BindFormActor(coop::element::ElementId oldEid, void* newActor,
                                       int32_t newIdx, coop::element::ElementId newEid, Form newForm,
                                       const std::wstring& className,
                                       float locX, float locY, float locZ,
                                       float rotPitch, float rotYaw, float rotRoll);

// Host only: the verb was refused (a sentient or kill-flagged kerfur; the old-form actor is
// still live and no new form spawned). Broadcast a rejected conversion carrying the old form
// and transform, so a client that optimistically converted its own mirror locally can
// restore it. The host table is unchanged. Game thread.
void BroadcastConvertRejected(coop::element::ElementId oldEid, Form oldForm,
                              float locX, float locY, float locZ,
                              float rotPitch, float rotYaw, float rotRoll,
                              const std::wstring& className);

// The same drop, addressed by the dying form's wire eid, for the death seams that hold one and
// not the kerfur id. A no-op unless the eid is still a tracked kerfur's CURRENT form, which is
// what keeps a conversion safe: the form bind moves the record to the successor's eid, so a late
// call naming the old one finds nothing to drop. The callers are the conversion converge, on the
// branches where no successor appeared, the kerfur first refusal when no verb bracket is open,
// and the NPC destroy PRE for a visible destroy.
void ReleaseKerfurForEid(coop::element::ElementId currentEid);

// Clear all per-session state (the host table and the client maps). The net disconnect. Game
// thread.
void OnDisconnect();

}  // namespace coop::kerfur_entity
