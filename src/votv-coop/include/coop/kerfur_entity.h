// coop/kerfur_entity.h -- the stable-identity layer for the kerfur (redesign 2026-06-16).
//
// THE kerfur root-cause fix. A kerfur is the only VOTV entity that is BOTH a host-authoritative AI
// NPC (kerfurOmega_C / ACharacter) AND a grabbable prop (prop_kerfurOmega_C / Aprop), flipped by a
// player turn_off (NPC->prop) / turn_on (prop->NPC) conversion. The game gives a kerfur NO stable
// cross-peer identity (AkerfurOmega_C::loadData mints a RANDOM key per peer per load; the conversion
// verbs copy no id), so the old design re-derived identity from scratch (class + nearest pose) on
// every conversion across two disjoint eid spaces -> the dupe-and-drop loop (7 failures, 1 root).
//
// FIX (MTA CElementRPCs SetElementModel shape): ONE host-allocated, host-range KerfurId per LOGICAL
// kerfur, spanning BOTH forms. See research/findings/votv-kerfur-sync-REDESIGN-2026-06-16.md, esp.
// the CORRECTED design in section 10 (post-RE: the radial-menu conversion is PE-invisible, so the
// client's local conversion is UNAVOIDABLE -> claim/adopt STAYS, anchored to this stable id; clients
// NEVER mint a kerfur eid).
//
// REGISTRY MODEL (section 10.2): a KerfurEntity is a HOST-ONLY authority element
// (ElementType::Kerfur, AllocHostId reserves the stable KerfurId K), held in a host-only table --
// NOT in MirrorManager<Npc/Prop>. The kerfur's RENDERED form is a normal Npc mirror (NPC form, eid
// N) or Prop mirror (prop form, eid P) at its own per-form host-range eid, in the existing managers,
// so the pose/physics pipelines are UNCHANGED. K is the durable handle; N/P are the per-form wire
// eids (at any instant only K + the current-form eid are live). The client tracks the kerfur with
// plain K<->eid maps (no Registry element -- the client is not the authority).
//
// PHASE: K-3 lands this table + its population/queries (no wire change; the conversion still runs the
// old path -- the table is built but not yet DRIVING). K-4 adds BindFormActor + the KerfurConvert
// broadcast. K-5 adds the client-mint class-gate + KerfurHoldRequest.

#pragma once

#include "coop/element/element.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::element {

// The host-side authority record reserving one stable host-range KerfurId per logical kerfur. NOT in
// MirrorManager<Npc/Prop> -- see the file header. Construction registers nothing by itself; the host
// table AllocHostId's it to reserve K, and frees K via ~Element when the record is erased.
class KerfurEntity : public Element {
public:
    KerfurEntity() : Element(ElementType::Kerfur) {}
};

}  // namespace coop::element

namespace coop::kerfur_entity {

// 0 = NPC form (kerfurOmega_C), 1 = prop form (prop_kerfurOmega_C).
enum class Form : uint8_t { Npc = 0, Prop = 1 };

// Cache the session pointer. Called from the coop subsystem Install fan-out (same point as
// kerfur_convert). AllocKerfurId reads role() through it; K-4's BindFormActor broadcasts through it.
void SetSession(coop::net::Session* session);

// Cache the resolved kerfur class UClass* pointers (kerfur_convert resolves them at its Install and
// calls this once they bind). Lets IsKerfurClass / IsKerfurActor answer via a subclass-aware walk
// without re-resolving. Safe to call repeatedly (idempotent on the same pointers).
void SetKerfurClasses(void* npcClass, void* propClass);

// True iff `cls` (a UClass*) derives from the kerfur NPC or prop base. False until SetKerfurClasses
// has bound the bases. Subclass-aware (kerfurOmega_col_C, prop_kerfurOmega_C skins, etc).
bool IsKerfurClass(void* cls);
// True iff `actor`'s class is a kerfur class. nullptr-safe.
bool IsKerfurActor(void* actor);

// HOST only: reserve (or return the existing) stable KerfurId for a kerfur `actor` first seen in
// `form`, whose current-form wire eid is `currentEid`. Idempotent per actor (returns the existing K
// if already tracked). Records actor->K and currentEid->K. Returns K, or kInvalidId on
// non-host / null actor / Registry exhaustion. Game thread.
coop::element::ElementId AllocKerfurId(void* actor, coop::element::ElementId currentEid,
                                       Form form, const std::wstring& className);

// HOST only: O(1) lookups (kInvalidId / default if not a tracked kerfur).
coop::element::ElementId GetKerfurIdForActor(void* actor);
coop::element::ElementId GetKerfurIdForEid(coop::element::ElementId currentEid);
coop::element::ElementId GetCurrentEidForKerfurId(coop::element::ElementId kerfurId);
Form                     GetFormForKerfurId(coop::element::ElementId kerfurId);

// HOST only: is this wire eid the CURRENT-form eid of a tracked kerfur? Used by the K-5 class-gate +
// the conversion routing. (The CLIENT is eid-based and does NOT track KerfurIds -- K-4b simplification
// per redesign section 11: it applies KerfurConvert by oldEid/newEid + IsKerfurClass, never by a
// KerfurId, so the client K<->eid maps + RegisterClientKerfur of the K-3 scaffolding were removed.)
bool IsKerfurEid(coop::element::ElementId currentEid);

// HOST only: the conversion mutation -- the MTA SetElementModel equivalent (redesign 10.3). The host
// ran the BP verb (its own radial menu OR a client KerfurConvertRequest) and registered the new-form
// actor SILENTLY via the normal Npc/Prop pipeline at host-range `newEid`. This rebinds the stable
// KerfurId (resolved from `oldEid`, or freshly allocated if the dying form was untracked) onto the
// new form IN PLACE -- K is PRESERVED across the conversion, never re-minted -- then broadcasts the
// SOLE conversion-transition packet KerfurConvert(K, oldEid, newEid, toForm, transform, className) to
// all peers. Returns K (kInvalidId on non-host / null actor / Registry exhaustion). The caller reads
// the new actor's transform + class and passes them (keeps this module's engine surface minimal).
// Game thread.
coop::element::ElementId BindFormActor(coop::element::ElementId oldEid, void* newActor,
                                       int32_t newIdx, coop::element::ElementId newEid, Form newForm,
                                       const std::wstring& className,
                                       float locX, float locY, float locZ,
                                       float rotPitch, float rotYaw, float rotRoll);

// HOST only: the verb was REFUSED (sentient/kill -- the old-form actor is still live, no new form
// spawned). Broadcast KerfurConvert(rejected=1) carrying the OLD form + transform so a client that
// optimistically converted its own mirror locally can RESTORE it (fixes Failure #7). The host table is
// unchanged (the kerfur did not convert). Game thread.
void BroadcastConvertRejected(coop::element::ElementId oldEid, Form oldForm,
                              float locX, float locY, float locZ,
                              float rotPitch, float rotYaw, float rotRoll,
                              const std::wstring& className);

// Drop a kerfur record (its actor died for good -- destroyed, not converted). HOST frees K. No-op on
// an untracked id. Game thread. (K-4 BindFormActor handles the CONVERSION case -- reuse, not drop.)
void ReleaseKerfurId(coop::element::ElementId kerfurId);

// Clear all per-session state (host table + client maps). Net disconnect. Game thread.
void OnDisconnect();

}  // namespace coop::kerfur_entity
