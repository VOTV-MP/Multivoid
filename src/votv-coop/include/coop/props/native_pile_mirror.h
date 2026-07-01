// coop/props/native_pile_mirror.h -- materialize a host-authoritative trash PILE mirror as a ROOTED
// real actorChipPile_C native (replacing the bare AStaticMeshActor proxy for the pile form).
//
// WHY (2026-06-30, the proxy->native nativization): the bare proxy (coop/trash_proxy) can NEVER be the
// game's lookAtActor (no int_player_C) and has no collision -> no native hover GUI, no movement-block,
// no occlusion-correct aim, wrong rotation. The proxy existed only because a client-SPAWNED real pile
// died on its own within ~10s (the 2026-06-21 dup). An inertness probe (2026-06-30) PROVED that death
// was GC (unrooted): a runtime-spawned actorChipPile_C with the recipe below stayed live + inert (no
// self-destruct, no self-morph) for 60s WITH collision ON, and showed the native hover GUI on aim. So a
// rooted runtime native is a safe, fully-native mirror. [[lesson-runtime-staticmeshactor-must-be-movable]]
//
// A materialized native is BOUND + MARKED save-native (Element::SetSaveNative) so it rides the EXACT
// same proven machinery as a save-loaded bound native -- pose drive (ResolveLiveActorByEid), b3
// position-correction, the grab route (OnPileGrabPre reads lookAtActor + GrabIntent), the morph hand-off
// (OnConvert pile->clump), the divergence-sweep exemption, and retire. The CLUMP form stays a bare proxy
// (it has a LifeSpan + autonomous re-pile-on-contact -> too live to keep as a native).
//
// GAME-THREAD only.

#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>
#include <string>

namespace ue_wrap { struct FVector; struct FRotator; }

namespace coop::native_pile_mirror {

// Spawn a rooted real chipPile native of `className` for trash eid `eid`, force the inert recipe
// (AddToRoot + tick-off + SimulatePhysics off + Movable), skin the `chipType` pile mesh (via the pile's
// own init(), which skins both mesh components), apply the HOST's authoritative `meshWorldRot` to the
// visible StaticMesh COMPONENT (so the client matches the host's pile roll instead of the native's own
// random UCS roll -- host->client, the same delivery axis as chipType), apply `scale`, and -- unless
// `skipBind` -- RegisterPropMirror it at `eid` (rebindInPlace per `rebindInPlace`) + mark it save-native
// so the bound-native machinery adopts it. `meshWorldRot` is the host's captured GetVisibleMeshWorldRotation
// (the wire rotation). Returns the native actor, or nullptr on failure. Game thread.
void* Materialize(coop::element::ElementId eid, const std::wstring& className, uint8_t chipType,
                  const ue_wrap::FVector& loc, const ue_wrap::FRotator& meshWorldRot,
                  const ue_wrap::FVector& scale, int senderSlot, bool skipBind, bool rebindInPlace);

// CLAIM an already-bound save-loaded native pile as the LAND mirror: reposition + re-skin it to the host's
// landed transform (loc + chipType + the host's visible-mesh `meshWorldRot` + scale). NO spawn, NO bind --
// the native is already the Element's bound mirror. This is the LAND-side symmetric half of the GRAB morph
// hand-off (remote_prop::OnConvert): on a re-pile LAND for an eid already bound to a save-loaded native, the
// native IS the correct resting form, so we reuse it instead of spawning a parallel proxy the duplicate-eid
// guard would reject (leaving a split-tracked dup the save-time sweep can't see). Game thread.
void RepositionBoundNative(void* native, uint8_t chipType, const ue_wrap::FVector& loc,
                           const ue_wrap::FRotator& meshWorldRot, const ue_wrap::FVector& scale);

}  // namespace coop::native_pile_mirror
