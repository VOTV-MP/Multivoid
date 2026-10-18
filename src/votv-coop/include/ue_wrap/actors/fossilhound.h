// ue_wrap/actors/fossilhound.h -- engine access for the fossilhound (fossilhound_C). What a client does for its
// mirror of one, whose own Blueprint graph is refused: the part of BeginPlay that is presentation (the mesh's
// parent, the animation instance and its owner) and the start of the per-viewer loop the class runs from the local
// camera. Engine-wrapper layer (principle 7): reflected reads, writes and calls, with no gameplay, network or coop
// state. Game thread. Implementation: src/ue_wrap/actors/fossilhound.cpp.

#pragma once

#include <cstdint>

namespace ue_wrap::fossilhound {

// The entry of the class's event graph that runs the per-viewer loop: the camera-distance test and the event
// count it moves, which re-enters itself on a 0.25 s delay. A cooked byte offset, so a game update moves it.
inline constexpr int32_t kPerViewerEntry = 2723;

// BeginPlay's presentation, reproduced in its order: the mesh re-attached to the spring arm (the class's
// construction parents it to the capsule), the mesh's animation instance written into `anim` and `anim.owner` set to
// the hound, only when that instance's class is, or derives from, the class's own AnimBP; then the graph entered at
// kPerViewerEntry. True when every step ran; false, and said once per class, when one did not resolve or its call
// faulted (the later steps still run). Game thread.
bool ApplyBeginPlayPresentation(void* hound);

}  // namespace ue_wrap::fossilhound
