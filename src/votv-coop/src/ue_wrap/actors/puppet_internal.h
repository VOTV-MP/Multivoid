// ue_wrap/actors/puppet_internal.h -- IMPLEMENTATION-PRIVATE shared seam between
// puppet.cpp and puppet_spawn.cpp ONLY (s28 cut 2026-07-19).
//
// NOT a public header (lives under src/, not include/): raw-offset access templates + the
// mesh-component cache the two puppet TUs share, none of it part of the public puppet.h
// surface. Same family pattern as coop/props/remote_prop_internal.h and
// coop/creatures/npc_sync_internal.h. Do NOT include from outside the puppet TU family.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace ue_wrap::puppet {

// Read/write a UObject* / POD at a byte offset (raw engine memory access is allowed in the
// wrapper layer). The offset is signed and a NEGATIVE one is refused, because half these call
// sites pass a reflection-resolved offset, which is -1 until its blueprint class loads and stays
// -1 if a recook renamed the field. Widened to size_t that sentinel addressed one byte BELOW the
// object -- silent garbage out of a read, a corrupted object header out of a write -- so the
// refusal lives here, where every caller already goes, rather than in a check each one can
// forget. A refused read yields a zeroed value and a refused write does nothing.
inline void* ReadPtr(void* base, ptrdiff_t off) {
    if (!base || off < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off);
}
template <class T>
inline T ReadAt(void* base, ptrdiff_t off) {
    if (!base || off < 0) return T{};
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}
template <class T>
inline void WriteAt(void* base, ptrdiff_t off, T value) {
    if (!base || off < 0) return;
    *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off) = value;
}

// puppet actor -> cached SkeletalMeshComponent. DEFINED in puppet.cpp (its owner/reader,
// GetSkeletalMeshComponent); puppet_spawn.cpp fills the cache once at spawn.
extern std::unordered_map<void*, ue_wrap::CachedObjRef> g_meshComp;

// The live AnimInstance running on a SkeletalMeshComponent (comp + AnimScriptInstance).
// Defined in puppet.cpp; the spawn path reads it during the orphan rig-up.
void* LiveAnimInstance(void* skeletalMeshComponent);

}  // namespace ue_wrap::puppet
