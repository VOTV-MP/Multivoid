// coop/sync/sync_create.h -- CreateOrAdopt, the ONE collision-reconcile create/bind path.
//
// THE keystone of the consolidated sync module (plan section 1, the MTA
// `Packet_EntityAdd` analog: resolve ID -> if occupied, retire-stale ->
// construct-with-ID). Every wire-received entity bind funnels through here so
// the "is this eid already taken, by the same actor or a divergent one" decision
// has ONE owner instead of being smeared across RegisterPropMirror / the OnSpawn
// dedup ladder / the OnConvert morph hand-off. That smear is the mirror-identity-
// race class (D1/D2); CreateOrAdopt is where it converges.
//
// Game-thread: touches engine reflection (InternalIndexOf) + the kerfur held-pose
// notify. The registry / manager ops are mutex-guarded internally.

#pragma once

#include <string>

#include "coop/element/element.h"  // ElementId

namespace coop::sync {

// CreateOrAdopt for a wire-received PROP MIRROR. Resolves `eid` against the
// registry and CLAIMs / ADOPTs / REJECTs:
//   - eid already bound to `actor`            -> ADOPT (idempotent no-op).
//   - eid bound to a DIFFERENT live actor:
//        morph=true   -> RE-SKIN the Element onto `actor` (a MIRROR rebinds via
//                        SetActor; a LOCAL element routes through
//                        RebindLocalElementActor so the forward map stays
//                        consistent). The caller retires the old actor.
//        morph=false  -> REJECT (the HEAD live-conflict guard); the existing
//                        binding is left intact (a re-spawn convergence packet
//                        for a still-live eid is absorbed, not duplicated).
//   - eid free                                -> CREATE a fresh Prop mirror
//                                                Install'd at `eid`.
// `eid == 0` (wire sentinel "sender had no Element minted") and `kInvalidId` are
// rejected. `senderSlot` tags the owner peer slot for per-slot disconnect
// eviction. Subsumes remote_prop::RegisterPropMirror (now a thin caller).
void CreateOrAdoptPropMirror(coop::element::ElementId eid, void* actor,
                             const std::wstring& key, const std::wstring& cls,
                             int senderSlot, bool morph);

// CreateOrAdopt for a wire-received NPC MIRROR (the client binds the host's NPC
// eid to a local actor). Same single-owner bind decision as the prop keystone,
// minus the prop-only concerns (no save Key, no morph re-skin -- an NPC that
// changes form is a kerfur and routes through kerfur_convert, not a morph here;
// no kerfur held-pose notify). Returns true iff the mirror is now bound to
// `actor` (idempotent re-bind of the same actor returns true; a different live
// actor already at `eid` is REJECTED -> false, the HEAD live-conflict guard).
// The caller does the NPC-specific post-bind setup (tick-disable / AI-neutralize).
bool CreateOrAdoptNpcMirror(coop::element::ElementId eid, void* actor,
                            const std::wstring& cls, int senderSlot);

// CreateOrAdopt for a wire-received WORLDACTOR MIRROR (UFOs/ships/jellyfish --
// the non-Character host-authoritative event actors). Same contract as the NPC
// funnel. The caller does the WorldActor-specific post-bind setup.
bool CreateOrAdoptWorldActorMirror(coop::element::ElementId eid, void* actor,
                                   const std::wstring& cls, int senderSlot);

}  // namespace coop::sync
