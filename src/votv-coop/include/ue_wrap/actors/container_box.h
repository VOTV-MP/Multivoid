// ue_wrap/actors/container_box.h -- a world container's overlap sensor: prop_container_C's `overlap` box, the volume a
// simulating prop enters to be filed (its BeginOverlap runs putObjectIn_overlap). Engine-wrapper layer (principle 7):
// reads and calls, no judgement and no coop state. Every reflected offset is resolved once per class and cached. Game
// thread. Implementation: src/ue_wrap/actors/container_box.cpp.

#pragma once

namespace ue_wrap::container_box {

// The container's `overlap` component, live, or null for a null or dead container, a class without the field and a box
// that is gone.
void* OverlapComponent(void* container);

// Is `actor` a prop_roomba_C, a prop_container_C subclass whose own upd() writes its box every tick on every peer?
bool IsRoomba(void* actor);

}  // namespace ue_wrap::container_box
