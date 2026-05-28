// coop/element/npc.h -- the Npc Element subclass.
//
// First subclass of `coop::element::Element`. PoC for the MTA CClientEntity
// adoption per `research/findings/votv-mta-cclientelement-audit-2026-05-28.md`
// section 4.5. NPCs were chosen as the PoC target because (a) lifecycle is the
// simplest (host spawns, host destroys, no per-frame ownership transfer); (b)
// the prior `g_nextNpcSessionId` atomic counter is the closest semantic match
// to MTA's ElementID free-list; (c) the smallest "before" footprint of the
// three replicated entity classes.
//
// Owned by `coop::npc_sync` -- the interceptor on the host side allocates an
// Npc per host-spawned NPC class and binds the actor pointer when the spawn
// POST completes (post-PoC; in PoC the actor pointer stays nullptr because the
// POST observer isn't yet wired -- the ElementId is allocated for wire
// addressing only). Destroyed by the same subsystem when the engine
// K2_DestroyActor PRE observer for the NPC fires (also post-PoC -- in PoC,
// elements accumulate until OnDisconnect resets the Registry).
//
// Future expansion (not in PoC, captured here for design clarity):
//   - Wire the POST observer on BeginDeferredSpawnFromClass to capture the
//     returned AActor* and stash it in `Element::SetActor`.
//   - Wire a K2_DestroyActor PRE observer (gated on `IsClassOrDerivedFromAny
//     Allowlisted`) to destroy the Npc element when the engine destroys the
//     actor.
//   - Replace `EntitySpawnPayload.sessionId` (uint32_t, named for the legacy
//     scheme) with `EntitySpawnPayload.elementId` semantically (same uint32_t
//     wire type; rename only) at the v12 protocol bump.

#pragma once

#include "coop/element/element.h"

namespace coop::element {

class Npc : public Element {
public:
    Npc() : Element(ElementType::Npc) {}
};

}  // namespace coop::element
