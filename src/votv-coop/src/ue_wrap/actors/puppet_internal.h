// ue_wrap/actors/puppet_internal.h -- IMPLEMENTATION-PRIVATE shared seam between
// puppet.cpp and puppet_spawn.cpp ONLY (s28 cut 2026-07-19).
//
// NOT a public header (lives under src/, not include/): raw-offset access templates + the
// mesh-component cache the two puppet TUs share, none of it part of the public puppet.h
// surface. Same family pattern as coop/props/remote_prop_internal.h and
// coop/creatures/npc_sync_internal.h. Do NOT include from outside the puppet TU family.

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace ue_wrap::puppet {

// Read/write a UObject* / POD at a fixed byte offset (raw engine memory access is
// allowed in the wrapper layer; offsets come from sdk_profile.h, verified vs the
// CXX dump).
inline void* ReadPtr(void* base, size_t off) {
    return base ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off) : nullptr;
}
template <class T>
inline T ReadAt(void* base, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}
template <class T>
inline void WriteAt(void* base, size_t off, T value) {
    *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off) = value;
}

// puppet actor -> cached SkeletalMeshComponent. DEFINED in puppet.cpp (its owner/reader,
// GetSkeletalMeshComponent); puppet_spawn.cpp fills the cache once at spawn.
extern std::unordered_map<void*, void*> g_meshComp;

// The live AnimInstance running on a SkeletalMeshComponent (comp + AnimScriptInstance).
// Defined in puppet.cpp; the spawn path reads it during the orphan rig-up.
void* LiveAnimInstance(void* skeletalMeshComponent);

}  // namespace ue_wrap::puppet
