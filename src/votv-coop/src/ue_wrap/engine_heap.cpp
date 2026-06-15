// ue_wrap/engine_heap.cpp -- engine-heap (GMalloc) allocate / free.
//
// Split from reflection.cpp (2026-06-14): the EngineAlloc/EngineFree allocator pair + the GMalloc
// slot resolution are a self-contained concern, logically distinct from the GUObjectArray / GNames
// / ProcessEvent reflection primitives -- and adding EngineAlloc (inventory Inc 4) pushed
// reflection.cpp past the 800-LOC soft cap. The PUBLIC API (EngineAlloc / EngineFree) is declared
// in reflection.h and stays in the ue_wrap::reflection namespace (callers are unchanged); only the
// implementation + the &GMalloc slot move here. ResolveEngineHeap() is called once, eagerly, from
// reflection::Resolve() alongside the other primitives -- behavior is byte-identical to before.
//
// Mechanism: decode &GMalloc from the FMemory::Realloc signature (that wrapper is `return
// GMalloc->Realloc(...)`), keep the SLOT (deref at use time -- GMalloc is set once early in boot
// and never moves), and call FMalloc::Free / FMalloc::Realloc via their vtable byte offsets
// (IDA-verified for this UE4.27 build; sdk_profile.h kFMallocFreeVtOff +0x30 / kFMallocReallocVtOff
// +0x20). Allocator-matched: a buffer EngineAlloc'd is freed by the engine's later Array realloc /
// GC with the same GMalloc, and EngineFree releases engine-allocated buffers, with no mismatch.

#include "ue_wrap/reflection.h"

#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/sig_scan.h"

#include <cstddef>
#include <cstdint>

namespace ue_wrap::reflection {
namespace {
namespace P = profile;

// &GMalloc (the slot holding the FMalloc*), decoded from FMemory::Realloc. We keep the SLOT, not
// the value, and deref at use time -- GMalloc is set once early in engine boot and never moves,
// but reading it live is strictly safe.
void** g_gmallocSlot = nullptr;

}  // namespace

void ResolveEngineHeap() {
    if (g_gmallocSlot) return;
    const uintptr_t hit = FindPattern(P::kSigFMemoryRealloc);
    if (!hit) return;
    const int32_t disp = *reinterpret_cast<int32_t*>(hit + P::kGMallocLeaDispOff);
    g_gmallocSlot = reinterpret_cast<void**>(hit + P::kGMallocLeaEndOff + disp);
}

void EngineFree(void* enginePtr) {
    if (!enginePtr || !g_gmallocSlot) return;
    void* gmalloc = *g_gmallocSlot;  // FMalloc* (live read; set once at engine boot)
    if (!gmalloc) return;
    // FMalloc::Free(this, ptr) -- vtable slot at byte offset kFMallocFreeVtOff.
    void** vtbl = *reinterpret_cast<void***>(gmalloc);
    using FreeFn = void(__fastcall*)(void* self, void* ptr);
    auto freeFn = reinterpret_cast<FreeFn>(vtbl[P::kFMallocFreeVtOff / sizeof(void*)]);
    freeFn(gmalloc, enginePtr);
}

void* EngineAlloc(size_t size, uint32_t align) {
    if (!size || !g_gmallocSlot) return nullptr;
    void* gmalloc = *g_gmallocSlot;  // FMalloc* (live read; set once at engine boot)
    if (!gmalloc) return nullptr;
    // FMalloc::Realloc(this, Original=nullptr, Count, Alignment) allocates when Original is null.
    // We use the Realloc slot the kSigFMemoryRealloc signature is matched FROM (verified-in-use),
    // never the unexercised Malloc slot. Allocator-matched with EngineFree.
    void** vtbl = *reinterpret_cast<void***>(gmalloc);
    using ReallocFn = void*(__fastcall*)(void* self, void* original, size_t count, uint32_t alignment);
    auto reallocFn = reinterpret_cast<ReallocFn>(vtbl[P::kFMallocReallocVtOff / sizeof(void*)]);
    return reallocFn(gmalloc, nullptr, size, align);
}

}  // namespace ue_wrap::reflection
