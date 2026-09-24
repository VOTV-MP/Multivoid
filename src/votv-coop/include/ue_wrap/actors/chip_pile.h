// ue_wrap/actors/chip_pile.h -- engine access for the look of a chip pile (actorChipPile_C and its
// variants). Principle-7 engine-wrapper layer: no network or coop state.
//
// A pile owns two static mesh components. `collision` is the ROOT: its transform is the actor's,
// and it is what the save keeps. `StaticMesh` is its child and is the mesh a player sees: the
// pile's init() gives it a random relative rotation on every construction, a save load included,
// and nothing saves that. So a pile's visible orientation is decided anew by each process that
// constructs it, and the actor's rotation says nothing about it. The component is reached through
// the actor's own `StaticMesh` field, by offset: asking for "a static mesh component" of the actor
// answers with the root.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::chip_pile {

// The component a player sees, or null when `actor` is not a live pile. Game thread.
void* VisibleMesh(void* actor);

// The visible mesh's world rotation; the actor's rotation when `actor` has no such mesh. False
// when that actor rotation could not be read (the mesh's own read has no failure to report yet).
// Game thread.
bool VisibleMeshWorldRotation(void* actor, FRotator& out);

// The visible mesh's transform relative to the root: what init() draws at random.
struct Look {
    FRotator relRotation;
    FVector  relScale;
};

// Read `actor`'s look off its visible mesh. False when `actor` has no such mesh or the scene
// component's layout did not resolve. Game thread.
bool ReadLook(void* actor, Look& out);

// Give `actor`'s visible mesh `look`, through the same two verbs init() uses. init() leaves the
// component Static, and the engine refuses to move a Static component in a running world, so it is
// made Movable for the write and Static again after it, as init() itself does. False when `actor`
// has no such mesh. Game thread.
bool ApplyLook(void* actor, const Look& look);

}  // namespace ue_wrap::chip_pile
