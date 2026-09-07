// coop/props/native_pile_mirror.h -- materialize a host-authoritative trash PILE mirror as a ROOTED
// real actorChipPile_C native, in place of a bare AStaticMeshActor proxy.
//
// A bare proxy can never be the game's lookAtActor, having no int_player_C, and has no collision --
// so no native hover GUI, no movement block, no occlusion-correct aim and the wrong rotation. It
// existed only because a client-SPAWNED real pile used to die on its own within about ten seconds.
// That death was GC, from being unrooted: a runtime-spawned actorChipPile_C built by the recipe
// below stays live and inert -- no self-destruct, no self-morph -- for a minute with collision ON,
// and shows the native hover GUI on aim.
//
// A materialized native is BOUND and MARKED save-native (Element::SetSaveNative), so it rides the
// same machinery as a save-loaded bound native: the pose drive through ResolveLiveActorByEid,
// position correction, the grab route, the morph hand-off in OnConvert from pile to clump, the
// divergence-sweep exemption, and retire. The CLUMP form stays a bare proxy -- it has a LifeSpan
// and re-piles on contact by itself, too live to keep as a native. GAME-THREAD only.

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

// Release the GC pin Materialize took on `actor`, if we took one. No-op for a save-loaded
// native or a game-native we never pinned -- so a caller that is about to destroy an actor
// of unknown provenance can call it unconditionally. Replaces the raw RemoveFromRoot the
// destroy/convert paths used to spell out: the pin has an OWNER now (see ue_wrap/core/gc_pin.h).
// Game thread.
void Unpin(void* actor);

// Drop every pin this module still holds. Called from the session teardown: a materialized
// native that is simply still alive when the session ends has no destroy path to ride, and a
// pin nobody releases anchors its whole world's Outer chain -- the shape that made a rejoin
// crash. Game thread.
void OnDisconnect();

}  // namespace coop::native_pile_mirror
