// coop/prop_synth_key.cpp -- see coop/prop_synth_key.h.
//
// Extracted from prop_lifecycle.cpp 2026-05-29 (M-1, post-E-2). The 4 file-
// local symbols (g_synthKeyCounter, g_setKeyFnMutex, g_setKeyFnByClass,
// ResolveSetKeyFn) are now in this file's anonymous namespace; only
// EnsureKeyForBroadcast is exposed.

#include "coop/props/prop_synth_key.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <unordered_map>

namespace coop::prop_synth_key {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

std::atomic<uint64_t> g_synthKeyCounter{0};

// Per-class setKey UFunction cache. setKey is a BP UFunction declared on the
// LINEAGE ROOT, not on every subclass: Aprop_C declares its own (Key @0x02E0),
// actor_save_C declares its own (key @0x0230, the trashBitsPile family), and
// FindFunction is exact-owner ([[lesson-findfunction-exact-owner-no-superstruct-climb]],
// 3rd strike: the take-4 "setKey not found on trashBitsPile_C" x162 re-key
// failure). Resolve by CLIMBING the SuperStruct chain to the declaring
// ancestor; cache per STARTING class (null cached too -- don't re-walk).
std::mutex g_setKeyFnMutex;
std::unordered_map<void*, void*> g_setKeyFnByClass;

void* ResolveSetKeyFn(void* cls) {
    if (!cls) return nullptr;
    std::lock_guard<std::mutex> lk(g_setKeyFnMutex);
    auto it = g_setKeyFnByClass.find(cls);
    if (it != g_setKeyFnByClass.end()) return it->second;
    void* fn = nullptr;
    void* hop = cls;
    for (int i = 0; i < 16 && hop && !fn; ++i) {
        fn = R::FindFunction(hop, P::name::PropSetKeyFn);  // "setKey"
        hop = R::SuperStructOf(hop);
    }
    g_setKeyFnByClass[cls] = fn;
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

std::wstring MintFreshKeyForDuplicate(void* self) {
    if (!self) return L"";
    void* cls = R::ClassOf(self);
    // ResolveSetKeyFn climbs to the declaring ancestor itself (Aprop_C for the
    // prop lineage, actor_save_C for trashBitsPile -- the take-4 x162 failure).
    void* setKeyFn = ResolveSetKeyFn(cls);
    if (!setKeyFn) {
        UE_LOGW("synth-key: rekey -- setKey UFunction not found on '%ls' (nor any SuperStruct ancestor) -- cannot re-key duplicate",
                R::ClassNameOf(self).c_str());
        return L"";
    }
    // Random (not counter-based like cs_): a re-key PERSISTS into the game's save,
    // so it must stay unique across boots -- a counter restarting at 0 could
    // re-mint a key already baked into an older record. 64 random bits is
    // collision-free at this project's key volumes. GT-only caller (census walk)
    // but keep the RNG mutex-guarded for the cost of nothing.
    static std::mutex sRngMutex;
    uint64_t r;
    {
        std::lock_guard<std::mutex> lk(sRngMutex);
        static std::mt19937_64 sRng([] {
            std::random_device rd;
            return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd()) ^
                   static_cast<uint64_t>(
                       std::chrono::steady_clock::now().time_since_epoch().count());
        }());
        r = sRng();
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"rk_%016llx", static_cast<unsigned long long>(r));
    const R::FName keyFName = ue_wrap::fname_utils::StringToFName(buf);
    if (keyFName.ComparisonIndex == 0) {
        UE_LOGW("synth-key: rekey StringToFName('%ls') -> NAME_None; cannot re-key", buf);
        return L"";
    }
    ue_wrap::ParamFrame sk(setKeyFn);
    if (!sk.SetRaw(L"Key", &keyFName, sizeof(keyFName))) {
        UE_LOGW("synth-key: rekey setKey 'Key' param not found on '%ls'",
                R::ClassNameOf(self).c_str());
        return L"";
    }
    if (!ue_wrap::Call(self, sk)) {
        UE_LOGW("synth-key: rekey setKey ProcessEvent call failed on '%ls'",
                R::ClassNameOf(self).c_str());
        return L"";
    }
    // Confirm by re-read: return the actual live key (L"" if the write did not take).
    const std::wstring got = ue_wrap::prop::GetInteractableKeyString(self);
    return (got == buf) ? got : L"";
}

}  // namespace coop::prop_synth_key
