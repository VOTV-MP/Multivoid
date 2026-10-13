// coop/props/trash_mirror.h -- the client's mirror of a trash entity, in either form, as the
// GAME's own actor with its brain parked.
//
// A rooted runtime actorChipPile_C or prop_garbageClump_C: GC-pinned, tick off, physics off, root
// movable, skinned from the host's chip type through the actor's own init, and bound as the
// element's mirror marked save-native, so it rides the same machinery as a save-loaded native.
// A real actor is what the game's look-at trace accepts, so the hover prompt, collision,
// occlusion and per-instance rotation are the game's.
//
// What makes keeping the game's actor safe is coop/props/trash_morph_gate, which refuses the
// three verbs a trash actor uses to author its own morph; tick-off alone would leave those bodies
// reachable. A clump additionally has its collision off for its whole life, carried and in
// flight -- the pile it lands as is a fresh actor with the game's own collision. GAME-THREAD only.
#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>
#include <string>

namespace ue_wrap { struct FVector; struct FRotator; }

namespace coop::trash_mirror {

// Spawn the mirror of trash eid `eid` in the form `className` names -- a pile or a clump -- with
// the inert recipe above, skin the `chipType` appearance (through the actor's own init, which
// sets the pile's mesh and the clump's material), give it the host actor's rotation `rot` and
// `scale`, and -- unless `skipBind` -- RegisterPropMirror it at `eid` (rebindInPlace per
// `rebindInPlace`) and mark it save-native. The turn a player sees on a pile is not `rot`: it is
// the child mesh's random draw, which arrives as the pile's look and is applied at the bind
// (coop/props/pile_look.h). Returns the actor, or nullptr on failure. Game thread.
// `resolve` governs the class-name lookup: null = reflection::FindClass (a whole-array walk on a
// miss); the host receiving a CLIENT's spawn/convert passes object_index::ClassByName so an
// attacker's unknown class costs a lookup, not a walk. A client keeps the default.
void* Materialize(coop::element::ElementId eid, const std::wstring& className, uint8_t chipType,
                  const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot,
                  const ue_wrap::FVector& scale, int senderSlot, bool skipBind, bool rebindInPlace,
                  void* (*resolve)(const wchar_t*) = nullptr);

// CLAIM an already-bound native pile as the LAND mirror: reposition and re-skin it to the host's
// landed transform (loc + chipType + the host actor's rotation `rot` + scale). NO spawn, NO
// bind -- the native is already the element's bound mirror. This is the LAND-side symmetric half of
// the GRAB morph hand-off: on a re-pile LAND for an eid already bound to a save-loaded native, that
// native IS the correct resting form, so it is reused instead of spawning a parallel one the
// duplicate-eid guard would reject, which would leave a split-tracked pair the save-time sweep
// cannot see. Game thread.
void RepositionBoundNative(void* native, uint8_t chipType, const ue_wrap::FVector& loc,
                           const ue_wrap::FRotator& rot, const ue_wrap::FVector& scale);

// Release the GC pin Materialize took on `actor`, if we took one. No-op for a save-loaded native
// or a game-native we never pinned -- so a caller about to destroy an actor of unknown provenance
// can call it unconditionally. Game thread.
void Unpin(void* actor);

// Retire the mirror of `eid`: evict its drives, unbind the element, and destroy the actor.
//
// `authoritative` says whose word this is. False -- a peer dropping, a session ending -- destroys
// only an actor THIS MODULE MADE: a client's own save-loaded pile is bound as a mirror too, and
// the client keeps its world when the session goes. True is the host's word that the entity is
// gone, and then the actor goes whoever made it, or the client is left with a pile the host does
// not have and no identity to reach it by. Game thread.
void Retire(coop::element::ElementId eid, bool authoritative);

// Did this module make `actor`? The question the drive layer asks before treating a trash actor
// as a mirror it owns rather than as one of the client's own world actors. Game thread.
bool WeMade(void* actor);

// Retire every mirror made for `slot`'s expressions (a single peer dropping while the session
// stays up). Must run before the generic per-slot mirror drain, which unbinds the elements and so
// takes away the only route from an eid back to its actor. Game thread.
void OnDisconnectForSlot(int slot);

// Destroy every mirror this module made and drop its pins. Called from the session teardown: a materialized mirror
// that is simply still alive when the session ends has no destroy path to ride, and a pin nobody
// releases anchors its whole world's Outer chain -- the shape that made a rejoin crash. Game
// thread.
void OnDisconnect();

}  // namespace coop::trash_mirror
