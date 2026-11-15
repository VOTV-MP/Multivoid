// coop/props/wire_class.h -- a class named on the wire, and how a peer that has not loaded it gets it.
//
// The sending side puts the class's leaf name AND its content path on the wire (coop/net/wire_class_path.h), reading
// both off the live actor or class it holds. The receiving side resolves the name through this one seam: its lookup first,
// and on a miss the load of the path (ue_wrap/core/class_load.h), which roots what it loads. An old peer's name carries no
// path, and the seam then is the lookup alone, as before.
//
// Game thread only.

#pragma once

#include "coop/net/protocol.h"

#include <string>

namespace coop::wire_class {

using Lookup = void* (*)(const wchar_t*);

// Fill `cn` with a class's leaf name (the caller holds it) and, for a /Game/ Blueprint class, its content path.
void FillFromClass(coop::net::WireClassName& cn, const std::wstring& leaf, void* cls);

// The class `cn` names: `lookup` first (null = FindClass; the host passes object_index::ClassByName), then, on a miss, the
// load of the path the name carries. Null when neither finds it.
void* Resolve(const coop::net::WireClassName& cn, Lookup lookup = nullptr);

}  // namespace coop::wire_class
