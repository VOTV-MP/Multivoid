// ue_wrap/core/object_path.h -- an object's path as the engine writes it, built from its outers. Engine-wrapper layer
// (principle 7): no network, no coop state. The same text on every machine that runs the same build, which is what a
// table of sounds keyed by path relies on. Game thread.

#pragma once

#include <string>

namespace ue_wrap::object_path {

// "/Game/audio/x/y_Cue.y_Cue": the package, then the object's own name; a nested object adds ":" for each level
// below the first. Empty for a null object.
std::wstring Of(void* uobject);

}  // namespace ue_wrap::object_path
