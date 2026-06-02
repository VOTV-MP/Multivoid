// coop/prop_synth_key.cpp -- see coop/prop_synth_key.h.
//
// Extracted from prop_lifecycle.cpp 2026-05-29 (M-1, post-E-2). The 4 file-
// local symbols (g_synthKeyCounter, g_setKeyFnMutex, g_setKeyFnByClass,
// ResolveSetKeyFn) are now in this file's anonymous namespace; only
// EnsureKeyForBroadcast is exposed.

#include "coop/prop_synth_key.h"

#include "ue_wrap/call.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace coop::prop_synth_key {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

std::atomic<uint64_t> g_synthKeyCounter{0};

// Per-class setKey UFunction cache. setKey is a BP UFunction on each
// keyed-interactable class; FindFunction is O(GUObjectArray) so cache
// after first resolve.
std::mutex g_setKeyFnMutex;
std::unordered_map<void*, void*> g_setKeyFnByClass;

void* ResolveSetKeyFn(void* cls) {
    if (!cls) return nullptr;
    std::lock_guard<std::mutex> lk(g_setKeyFnMutex);
    auto it = g_setKeyFnByClass.find(cls);
    if (it != g_setKeyFnByClass.end()) return it->second;
    void* fn = R::FindFunction(cls, P::name::PropSetKeyFn);  // "setKey"
    g_setKeyFnByClass[cls] = fn;  // cache null too (don't re-walk)
    return fn;
}

}  // namespace

std::wstring EnsureKeyForBroadcast(void* self, const std::wstring& currentKey,
                                   bool mintForAprop) {
    if (!currentKey.empty() && currentKey != L"None") return currentKey;
    if (!mintForAprop && ue_wrap::prop::IsDescendantOfProp(self)) {
        // Aprop_C: trust the BP's own UCS to mint. Skip. (mintForAprop bypasses
        // this for the auto-grabbed trash-collect item whose UCS hasn't minted.)
        return currentKey;
    }
    if (!ue_wrap::prop::IsKeyedInteractable(self)) return currentKey;
    void* cls = R::ClassOf(self);
    void* setKeyFn = ResolveSetKeyFn(cls);
    if (!setKeyFn) {
        UE_LOGW("synth-key: setKey UFunction not found on '%ls' -- cannot mint",
                R::ClassNameOf(self).c_str());
        return currentKey;
    }
    const uint64_t n = g_synthKeyCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t ptrLow32 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    wchar_t buf[64];
    swprintf(buf, 64, L"cs_%08x_%llu", ptrLow32, static_cast<unsigned long long>(n));
    const R::FName keyFName = ue_wrap::fname_utils::StringToFName(buf);
    if (keyFName.ComparisonIndex == 0) {
        UE_LOGW("synth-key: StringToFName('%ls') -> NAME_None; cannot mint", buf);
        return currentKey;
    }
    ue_wrap::ParamFrame sk(setKeyFn);
    if (!sk.SetRaw(L"Key", &keyFName, sizeof(keyFName))) {
        UE_LOGW("synth-key: setKey 'Key' param not found on '%ls'",
                R::ClassNameOf(self).c_str());
        return currentKey;
    }
    if (!ue_wrap::Call(self, sk)) {
        UE_LOGW("synth-key: setKey ProcessEvent call failed on '%ls'",
                R::ClassNameOf(self).c_str());
        return currentKey;
    }
    UE_LOGI("synth-key: minted '%ls' for actor %p class='%ls'",
            buf, self, R::ClassNameOf(self).c_str());
    // Re-read via GetInteractableKey to confirm the field actually changed.
    return ue_wrap::prop::GetInteractableKeyString(self);
}

}  // namespace coop::prop_synth_key
