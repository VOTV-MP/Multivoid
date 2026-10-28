// ue_wrap/actors/geom_octa.h -- engine access for the Geom Octahedron (geomOcta_C) and its laser (geomOctaLazer_C):
// reflected reads of the members the coop layer judges them by. Engine-wrapper layer (principle 7): no gameplay,
// network or coop state. Game thread. Implementation: src/ue_wrap/actors/geom_octa.cpp.

#pragma once

#include "ue_wrap/core/script_gate.h"

namespace ue_wrap::geom_octa {

// The Octa's `targetActor` (the actor it is locked onto, set by its aggro and cleared when it closes), when that
// object is live; null for no target, a dead target, or an actor that is no Octa. Game thread.
void* TargetActor(void* octa);

// The Octa's `isAnim` (true from its aggro until its close finishes); false when the member does not resolve. Game
// thread.
bool IsAnim(void* octa);

// The Octa's `minSphere` component (the sphere whose begin-overlap picks its target), when live; null otherwise. Game
// thread.
void* MinSphere(void* octa);

// The Octa's event graph function (ExecuteUbergraph_geomOcta), the frame a replay's own calls report as their caller;
// null for an actor that is no Octa. Game thread.
void* GraphFunction(void* octa);

// The class's own begin-overlap handler of minSphere, called as the engine calls it: `otherActor` (null for None) is the
// actor that began to overlap. The handler runs the Octa's aggro for it, unless the Octa is busy. The handler's body is
// refused on a marked mirror but for a pass (npc_mirror_brain::ScopedEntryPass), which the caller opens. False when the
// function or the call did not run. Game thread.
bool CallMinSphereBeginOverlap(void* octa, void* otherActor);

// The class's "fire" montage finish notify, called with the notify name `finish`, as the montage proxy calls it. The
// same condition as above. Game thread.
bool CallFireFinishNotify(void* octa);

// Is the montage notify this call reports named `finish`? For a script-gate watch on one of the class's notify functions
// (OnNotifyBegin_<guid>), which carries its name as the parameter NotifyName. Game thread.
bool IsFinishNotify(const script_gate::Call& call);

// The laser's owner (AActor::Owner, which the Octa's fire() sets to itself when it spawns the laser), when live; null
// otherwise. Game thread.
void* LaserOwner(void* laser);

}  // namespace ue_wrap::geom_octa
