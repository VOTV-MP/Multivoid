// ue_wrap/actors/container_box.h -- a world container's overlap sensor: prop_container_C's `overlap` box, the volume a
// simulating prop enters to be filed (its BeginOverlap runs putObjectIn_overlap), and that body itself. Engine-wrapper
// layer (principle 7): reads and calls, no judgement and no coop state. Every reflected offset is resolved once per
// class and cached. Game thread. Implementation: src/ue_wrap/actors/container_box.cpp.

#pragma once

namespace ue_wrap::container_box {

// The container's `overlap` component, live, or null for a null or dead container, a class without the field and a box
// that is gone.
void* OverlapComponent(void* container);

// Is `actor` a prop_roomba_C, a prop_container_C subclass whose own upd() writes its box every tick on every peer?
bool IsRoomba(void* actor);

// Does the container's `overlap` box overlap `actor` now (the engine's IsOverlappingActor)? False for a null or dead
// container or actor, a container without the box, and a call that failed.
bool OverlapsActor(void* container, void* actor);

// The container's own putObjectIn_overlap(actor, component, audio), run as the engine's overlap event runs it, with the
// container's own Audio component: it files `actor`, destroys it and plays the sound when the body's gates pass, and
// returns silently when one does not. True when the call ran, not when it filed anything. False for a null or dead
// container, a null actor or component, a class without the function or the Audio field, and a call that failed.
bool CallPutObjectInOverlap(void* container, void* actor, void* component);

}  // namespace ue_wrap::container_box
