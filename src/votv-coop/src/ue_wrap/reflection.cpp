#include "ue_wrap/reflection.h"

#include "ue_wrap/log.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/sig_scan.h"

#include <windows.h>

#include <atomic>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ue_wrap::reflection {
namespace {

// All version-specific knowledge lives in the profile (the porting surface).
namespace P = profile;
namespace O = profile::off;

using FNameToStringFn = void(__fastcall*)(const FName*, FString*);
using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);

uintptr_t g_objArray = 0;
FNameToStringFn g_fnameToString = nullptr;

// Throttled count of IsLive() calls that faulted reading a freed pointer (see
// IsLive). File-scope atomic so the SEH-guarded IsLive stays free of any local
// object that would need stack unwinding (C2712). Surfacing these matters: a
// fault means a cached UObject* was GC-freed -- expected for a stale cache that
// then re-resolves, but a high rate flags a deeper lifetime issue to chase.
std::atomic<uint64_t> g_isLiveFaultCount{0};
ProcessEventFn g_processEvent = nullptr;

}  // namespace

// The engine-heap allocator (EngineAlloc / EngineFree + the &GMalloc slot) lives in
// ue_wrap/engine_heap.cpp (modular split, 2026-06-14). ResolveEngineHeap() resolves the GMalloc
// slot once; Resolve() calls it eagerly below, alongside the other primitives.
void ResolveEngineHeap();

uintptr_t GUObjectArrayAddr() { return g_objArray; }
uintptr_t FNameToStringAddr() { return reinterpret_cast<uintptr_t>(g_fnameToString); }
uintptr_t ProcessEventAddr() { return reinterpret_cast<uintptr_t>(g_processEvent); }
bool IsResolved() { return g_objArray && g_fnameToString && g_processEvent; }

bool Resolve() {
    if (!g_fnameToString) {
        const uintptr_t hit = FindPattern(P::kSigFNameToString);
        if (hit) g_fnameToString = reinterpret_cast<FNameToStringFn>(hit);
    }
    if (!g_objArray) {
        const uintptr_t hit = FindPattern(P::kSigGUObjectArray);
        if (hit) {
            const int32_t disp = *reinterpret_cast<int32_t*>(hit + P::kGUObjArrayLeaDispOff);
            g_objArray = hit + P::kGUObjArrayLeaEndOff + disp;
        }
    }
    if (!g_processEvent) {
        const uintptr_t hit = FindPattern(P::kSigProcessEvent);
        if (hit) g_processEvent = reinterpret_cast<ProcessEventFn>(hit);
    }
    ResolveEngineHeap();  // &GMalloc (engine_heap.cpp) -- best-effort, not part of IsResolved()
    return IsResolved();
}

// COOP-ORIGIN dispatch latch (rng_roll_census, 2026-07-10). Every dispatch OUR code issues goes
// through this one choke point, so a thread-local depth counter cleanly discriminates "the mod
// called this native" from "the game's own BP called it" inside a UFunction interceptor -- the
// context object alone cannot (our re-arms set timers ON game objects, e.g. spaceRenderer).
// Depth (not bool): a nested dispatch fired from within a coop dispatch stays tagged. RAII so an
// unwind can't leave the flag stuck.
namespace {
thread_local int t_coopDispatchDepth = 0;
struct CoopDispatchScope {
    CoopDispatchScope() { ++t_coopDispatchDepth; }
    ~CoopDispatchScope() { --t_coopDispatchDepth; }
};
}  // namespace

bool InCoopDispatch() { return t_coopDispatchDepth > 0; }

bool CallFunction(void* object, void* function, void* params) {
    if (!g_processEvent || !object || !function) return false;
    CoopDispatchScope scope;
    g_processEvent(object, function, params);
    return true;
}

int32_t NumObjects() {
    if (!g_objArray) return 0;
    return *reinterpret_cast<int32_t*>(g_objArray + O::FUObjectArray_ObjObjects + O::Chunk_NumElements);
}

namespace {
// Address of the FUObjectItem (Object*, Flags, Cluster, Serial) for a slot, or
// null if the index is out of range / the chunk is unallocated.
uint8_t* ItemAt(int32_t index) {
    if (!g_objArray || index < 0) return nullptr;
    const uintptr_t objObjects = g_objArray + O::FUObjectArray_ObjObjects;
    if (index >= *reinterpret_cast<int32_t*>(objObjects + O::Chunk_NumElements)) return nullptr;
    auto** chunks = *reinterpret_cast<uint8_t***>(objObjects + O::Chunk_Objects);
    if (!chunks) return nullptr;
    uint8_t* chunk = chunks[index / O::ElemsPerChunk];
    if (!chunk) return nullptr;
    return chunk + static_cast<size_t>(index % O::ElemsPerChunk) * O::FUObjectItem_Stride;
}
}  // namespace

void* ObjectAt(int32_t index) {
    uint8_t* item = ItemAt(index);
    return item ? *reinterpret_cast<void**>(item) : nullptr;  // FUObjectItem.Object @ +0x00
}

bool AddToRoot(void* obj) {
    if (!obj) return false;
    const int32_t idx = *reinterpret_cast<int32_t*>(
        reinterpret_cast<uint8_t*>(obj) + O::UObject_InternalIndex);
    uint8_t* item = ItemAt(idx);
    if (!item) return false;
    int32_t& flags = *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags);
    flags |= 0x40000000;  // EInternalObjectFlags::RootSet (UE4.27)
    return true;
}

bool RemoveFromRoot(void* obj) {
    if (!obj) return false;
    const int32_t idx = *reinterpret_cast<int32_t*>(
        reinterpret_cast<uint8_t*>(obj) + O::UObject_InternalIndex);
    uint8_t* item = ItemAt(idx);
    if (!item) return false;
    int32_t& flags = *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags);
    flags &= ~0x40000000;  // clear EInternalObjectFlags::RootSet (UE4.27)
    return true;
}

int32_t InternalIndexOf(void* obj) {
    if (!obj) return -1;
    // Dereferences obj -- caller guarantees obj is live/mapped (see header).
    return *reinterpret_cast<int32_t*>(
        reinterpret_cast<uint8_t*>(obj) + O::UObject_InternalIndex);
}

bool IsLiveByIndex(void* obj, int32_t internalIdx) {
    if (!obj || internalIdx < 0) return false;
    // Reads ONLY the GUObjectArray slot at the cached index -- never obj's own
    // (possibly GC-freed) memory. A purged/recycled slot no longer points back
    // to obj, so the compare fails cleanly instead of dereferencing freed mem.
    uint8_t* item = ItemAt(internalIdx);
    if (!item || *reinterpret_cast<void**>(item) != obj) return false;  // slot empty/recycled
    // The slot check alone is NOT enough across a level transition: an actor the
    // engine is tearing down is flagged PendingKill (then Unreachable by GC) yet
    // still occupies its array slot until the purge completes. Calling UFunctions
    // on it -- GetActorLocation/SetViewTargetWithBlend/etc. -- is a use-after-free
    // (the tutorial-map load crashed here: read 0xffff...). Reject those flags so a
    // dying object reports not-live. (FUObjectItem.Flags @ +0x08; bit values per
    // UE4.27 EInternalObjectFlags -- matches UE4SS's own PendingKill guard.)
    const int32_t flags = *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags);
    constexpr int32_t kKillFlags = 0x10000000 /*Unreachable*/ | 0x20000000 /*PendingKill*/;
    return (flags & kKillFlags) == 0;
}

bool IsLive(void* obj) {
    if (!obj) return false;
    // SEH-guard the ONE read of obj's OWN memory. IsLive is THE primitive for
    // checking a cached UObject* that may have been GC-purged (g_netLocal, the
    // local-player cache in players::Registry::Local, held props, cached
    // singletons). When the purge has freed+unmapped obj, reading
    // obj->InternalIndex faults -- catch it and report not-live so the caller's
    // clear-and-rescan path runs instead of crashing. This makes the header's
    // documented crash-safe contract real, and (with the /EHa firewall) BREAKS
    // the per-tick re-fault loop: the bare read used to abort the whole tick
    // task BEFORE the caller could clear its stale cache, so the same dead
    // pointer re-faulted every tick (2026-05-30 4-peer smoke: 3779 absorbed AVs
    // + an 11 GB RSS balloon on one client). Now IsLive returns false, the
    // cache is cleared + re-resolved, and the loop ends.
    //
    // NOT a crutch: IsLive's job IS to answer "is this pointer live?" -- for a
    // freed pointer the answer is "no", and the SEH makes it return that
    // instead of crashing. Faults are surfaced (throttled WARN), not hidden.
    // Recycling caveat: if obj's address was reused by a NEW UObject the read
    // won't fault, but IsLiveByIndex's slot compare (*item == obj) still
    // rejects the impostor. For the 2000-prop snapshot, where recycling at
    // scale matters, callers capture the index up-front and use IsLiveByIndex
    // directly (see prop_snapshot.cpp).
    int32_t idx;
    __try {
        idx = *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(obj) + O::UObject_InternalIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const uint64_t n = g_isLiveFaultCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n % 1000) == 0) {
            UE_LOGW("reflection: IsLive caught AV reading freed pointer %p (fault #%llu) -- reporting not-live",
                    obj, static_cast<unsigned long long>(n));
        }
        return false;
    }
    return IsLiveByIndex(obj, idx);
}

const FName& NameOf(void* uobject) {
    return *reinterpret_cast<FName*>(reinterpret_cast<uint8_t*>(uobject) + O::UObject_NamePrivate);
}

void* ClassOf(void* uobject) {
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(uobject) + O::UObject_ClassPrivate);
}

void* OuterOf(void* uobject) {
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(uobject) + O::UObject_OuterPrivate);
}

namespace {
// Render `name` into the per-thread scratch FString and return a pointer to the
// characters (+ length via lenOut), or nullptr on failure. The buffer is REUSED
// across calls (engine grows it rarely; we never free UE-allocated memory) --
// this is the zero-allocation primitive under ToString/NameEquals/NameStartsWith.
// The returned pointer is valid only until the next render on this thread.
const wchar_t* RenderNameToScratch(const FName& name, int& lenOut) {
    lenOut = 0;
    if (!g_fnameToString) return nullptr;
    // thread_local (not static): per-thread scratch, so two threads rendering names
    // never race the same FString.
    thread_local FString scratch{nullptr, 0, 0};
    // CRITICAL (2026-06-13, the RAM balloon): FName::ToString(Out) does NOT reuse the
    // caller's buffer -- IDA-verified it does `Out.Data = 0` (no free) then allocates
    // a FRESH engine buffer every call. So the PREVIOUS render's buffer is orphaned
    // and leaks unless WE free it. This is the hot name primitive (every NameEquals /
    // ToString / FindFunction goes through here, ~10^5/s), so the unfreed orphan was
    // the multi-MB/s engine-heap leak. Free the prior buffer before the engine drops
    // the pointer; the engine reallocates below. (No-op until GMalloc is resolved.)
    if (scratch.Data) {
        EngineFree(scratch.Data);
        scratch.Data = nullptr;
        scratch.Max = 0;
    }
    scratch.Num = 0;
    g_fnameToString(&name, &scratch);
    if (!scratch.Data || scratch.Num <= 0) return nullptr;
    int len = scratch.Num;
    if (scratch.Data[len - 1] == L'\0') --len;  // Num counts the null terminator
    if (len <= 0) return nullptr;
    lenOut = len;
    return scratch.Data;
}
}  // namespace

std::wstring ToString(const FName& name) {
    int len = 0;
    const wchar_t* s = RenderNameToScratch(name, len);
    if (!s) return L"";
    return std::wstring(s, s + len);
}

// Name comparisons are case-INSENSITIVE: engine FNames compare by
// ComparisonIndex (case-insensitive), and the rendered string carries
// whatever casing was registered FIRST in this process -- a case-sensitive
// compare makes by-name lookups depend on package load order. Found
// 2026-06-12: FindFunction(kerfurOmega_C, L"actionName") returned null all
// session because some earlier-loaded class registered the name as
// "ActionName" (kerfur_convert never installed -> the conversion dupe).
// Two engine names can never differ only by case, so insensitive matching
// is strictly more correct, never less.
bool NameEquals(const FName& name, const wchar_t* expected) {
    if (!expected) return false;
    int len = 0;
    const wchar_t* s = RenderNameToScratch(name, len);
    if (!s) return expected[0] == L'\0';
    const size_t elen = ::wcslen(expected);
    return elen == static_cast<size_t>(len) && ::_wcsnicmp(s, expected, elen) == 0;
}

bool NameStartsWith(const FName& name, const wchar_t* prefix) {
    if (!prefix) return false;
    int len = 0;
    const wchar_t* s = RenderNameToScratch(name, len);
    if (!s) return prefix[0] == L'\0';
    const size_t plen = ::wcslen(prefix);
    return plen <= static_cast<size_t>(len) && ::_wcsnicmp(s, prefix, plen) == 0;
}

bool NameContains(const FName& name, const wchar_t* needle) {
    if (!needle) return false;
    const size_t nlen = ::wcslen(needle);
    if (nlen == 0) return true;
    int len = 0;
    const wchar_t* s = RenderNameToScratch(name, len);
    if (!s || static_cast<size_t>(len) < nlen) return false;
    // Bounded scan (the scratch is not guaranteed null-terminated after the
    // trim, so no wcsstr) -- needles here are a few chars, rendered names ~16.
    // Case-insensitive like NameEquals (FName casing = first registration).
    for (size_t i = 0; i + nlen <= static_cast<size_t>(len); ++i) {
        if (::_wcsnicmp(s + i, needle, nlen) == 0) return true;
    }
    return false;
}

std::wstring ClassNameOf(void* uobject) {
    void* cls = uobject ? ClassOf(uobject) : nullptr;
    if (!cls) return L"";
    return ToString(NameOf(cls));
}

namespace {
// className -> resolved UClass* cache for the by-class walk helpers. Primed on
// the first textual match DURING a normal walk (no separate resolve pass);
// thereafter the walk compares ClassOf(obj) against ONE pointer -- no name
// render at all (~250k engine renders/walk -> ~250k pointer compares). Entries
// are revalidated once per walk: a BP UClass dies on world unload and its
// address can be recycled, so a cached class must still be live AND still carry
// the expected name before a walk trusts it. Keyed by FNV-1a of the text with
// the full string stored for exact verification (hash collisions fall back to
// the uncached slow path -- correct, just slow). Mutex-guarded: walks run on
// the game thread but nothing forbids net-thread lookups.
struct CachedClass {
    std::wstring name;
    void* cls = nullptr;
};
std::mutex g_classCacheMu;
std::unordered_map<uint64_t, CachedClass> g_classCache;

uint64_t Fnv1a(const wchar_t* s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s) {
        h ^= static_cast<uint64_t>(*s);
        h *= 1099511628211ull;
    }
    return h;
}

// The (revalidated) cached UClass* for className, or nullptr if unresolved /
// stale / a hash collision. Call once per walk, NOT per object.
void* BeginClassWalk(const wchar_t* className, uint64_t& hashOut) {
    hashOut = Fnv1a(className);
    void* cls = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_classCacheMu);
        auto it = g_classCache.find(hashOut);
        if (it == g_classCache.end()) return nullptr;
        if (::wcscmp(it->second.name.c_str(), className) != 0) return nullptr;  // collision
        cls = it->second.cls;
    }
    // Revalidate OUTSIDE the lock (IsLive may SEH-probe a freed pointer; NameOf
    // render touches the engine name table). Stale -> drop so the walk re-primes.
    // Ordering is load-bearing: IsLive(cls) true means the GUObjectArray slot at
    // cls's InternalIndex still points back at cls, i.e. cls is live and mapped --
    // ONLY then is the unguarded NameOf(cls) deref in NameEquals safe (the &&
    // short-circuit is the use-after-free guard here).
    if (cls && IsLive(cls) && NameEquals(NameOf(cls), className)) return cls;
    std::lock_guard<std::mutex> lk(g_classCacheMu);
    auto it = g_classCache.find(hashOut);
    if (it != g_classCache.end() && it->second.cls == cls) it->second.cls = nullptr;
    return nullptr;
}

void PrimeClassWalk(const wchar_t* className, uint64_t hash, void* cls) {
    std::lock_guard<std::mutex> lk(g_classCacheMu);
    auto& e = g_classCache[hash];
    if (e.name.empty()) e.name = className;
    else if (::wcscmp(e.name.c_str(), className) != 0) return;  // collision: leave the first owner
    e.cls = cls;
}

// Per-object match for the by-class walkers. Fast path: pointer compare against
// the walk-cached class. Slow path (cache unresolved): render-compare, and
// prime the cache on the first hit so the REST of this walk and every later
// walk take the fast path.
inline bool ObjClassMatches(void* obj, const wchar_t* className, uint64_t hash, void*& want) {
    void* cls = ClassOf(obj);
    if (!cls) return false;
    if (want) return cls == want;
    if (!NameEquals(NameOf(cls), className)) return false;
    want = cls;
    PrimeClassWalk(className, hash, cls);
    return true;
}
}  // namespace

void* FindObject(const wchar_t* name, const wchar_t* className) {
    if (!name) return nullptr;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (!NameEquals(NameOf(obj), name)) continue;
        if (className) {
            void* cls = ClassOf(obj);
            if (!cls || !NameEquals(NameOf(cls), className)) continue;
        }
        return obj;
    }
    return nullptr;
}

void* FindClass(const wchar_t* className) {
    if (!className) return nullptr;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (!NameEquals(NameOf(obj), className)) continue;
        // Its meta-class identifies it as a class object. Every UClass-derived
        // meta-type ends in "Class" (Class, BlueprintGeneratedClass,
        // WidgetBlueprintGeneratedClass, AnimBlueprintGeneratedClass, DynamicClass,
        // LinkerPlaceholderClass, ...). Match the suffix so BP-generated classes
        // (UMG widgets, anim BPs) resolve too -- the exact-name match above already
        // excludes instances (which carry numeric suffixes). Only runs for the
        // handful of name matches, so the wstring here is off the hot path.
        const std::wstring meta = ClassNameOf(obj);
        if (meta.size() >= 5 && meta.compare(meta.size() - 5, 5, L"Class") == 0) {
            return obj;
        }
    }
    return nullptr;
}

void* FindFunction(void* owningClass, const wchar_t* funcName) {
    if (!owningClass || !funcName) return nullptr;
    uint64_t fnHash = 0;
    void* fnCls = BeginClassWalk(L"Function", fnHash);
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (OuterOf(obj) != owningClass) continue;
        if (!ObjClassMatches(obj, L"Function", fnHash, fnCls)) continue;
        if (NameEquals(NameOf(obj), funcName)) return obj;
    }
    return nullptr;
}

void* FindObjectByClass(const wchar_t* className) {
    if (!className) return nullptr;
    uint64_t hash = 0;
    void* want = BeginClassWalk(className, hash);
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (!ObjClassMatches(obj, className, hash, want)) continue;
        // Skip the CDO (the archetype, named "Default__<Class>"); we want a
        // real instance.
        if (NameStartsWith(NameOf(obj), L"Default__")) continue;
        return obj;
    }
    return nullptr;
}

void* FindClassDefaultObject(const wchar_t* className) {
    if (!className) return nullptr;
    std::wstring defName = L"Default__";
    defName += className;
    return FindObject(defName.c_str());
}

std::vector<ObjectRef> ChildObjectsOf(void* outer) {
    std::vector<ObjectRef> out;
    if (!outer) return out;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj || OuterOf(obj) != outer) continue;
        out.push_back({ToString(NameOf(obj)), ClassNameOf(obj), obj});
    }
    return out;
}

void* SuperStructOf(void* cls) {
    if (!cls) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(cls) + O::UStruct_SuperStruct);
}

bool IsDescendantOfAny(void* cls, void* const* bases, size_t nBases, int maxHops) {
    if (!cls || !bases || nBases == 0) return false;
    for (int hops = 0; hops < maxHops && cls; ++hops) {
        for (size_t i = 0; i < nBases; ++i) {
            if (bases[i] && bases[i] == cls) return true;
        }
        cls = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(cls) + O::UStruct_SuperStruct);
    }
    return false;
}

void DebugProbeSuperStructOffset() {
    void* actorCls = FindClass(L"Actor");
    void* objectCls = FindClass(L"Object");
    void* pawnCls = FindClass(L"Pawn");
    UE_LOGI("superstruct probe: Actor=%p Object=%p Pawn=%p", actorCls, objectCls, pawnCls);
    if (!actorCls || !objectCls) return;
    auto* base = reinterpret_cast<uint8_t*>(actorCls);
    for (size_t off = 0x28; off <= 0x80; off += 8) {
        void* q = *reinterpret_cast<void**>(base + off);
        if (q == objectCls) {
            UE_LOGI("  Actor[0x%02zx] == Object class -> SuperStruct offset = 0x%02zx", off, off);
        } else if (q == pawnCls && pawnCls) {
            UE_LOGI("  Actor[0x%02zx] == Pawn class (unexpected)", off);
        }
    }
}

int32_t CountObjectsByClass(const wchar_t* className) {
    if (!className) return 0;
    int32_t count = 0;
    uint64_t hash = 0;
    void* want = BeginClassWalk(className, hash);
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (!ObjClassMatches(obj, className, hash, want)) continue;
        if (NameStartsWith(NameOf(obj), L"Default__")) continue;  // skip CDO
        ++count;
    }
    return count;
}

std::vector<void*> FindObjectsByClass(const wchar_t* className) {
    std::vector<void*> out;
    if (!className) return out;
    uint64_t hash = 0;
    void* want = BeginClassWalk(className, hash);
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (!ObjClassMatches(obj, className, hash, want)) continue;
        if (NameStartsWith(NameOf(obj), L"Default__")) continue;  // skip CDO
        out.push_back(obj);
    }
    return out;
}

// ---- FProperty / FField walkers: EXTRACTED to reflection_props.cpp
// (2026-07-10 soft-cap extraction; same namespace, no shared private state).

namespace {

// Log the running exe's version + size and warn if it differs from the build
// the profile was derived against -- the first thing to check on a new release.
void LogGameVersion() {
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(::GetModuleHandleW(nullptr), exe, MAX_PATH);

    unsigned long long sizeBytes = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(exe, GetFileExInfoStandard, &fad)) {
        sizeBytes = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }

    // Exe file version (catches engine-version changes that shift offsets).
    unsigned ms = 0, ls = 0;
    DWORD dummy = 0;
    const DWORD vsz = ::GetFileVersionInfoSizeW(exe, &dummy);
    if (vsz) {
        std::vector<uint8_t> buf(vsz);
        if (::GetFileVersionInfoW(exe, 0, vsz, buf.data())) {
            VS_FIXEDFILEINFO* ffi = nullptr;
            UINT len = 0;
            if (::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi) {
                ms = ffi->dwFileVersionMS;
                ls = ffi->dwFileVersionLS;
            }
        }
    }

    UE_LOGI("target build: game=%s engine=%s", P::kTargetGameVersion, P::kTargetEngineVersion);
    UE_LOGI("running exe : %ls", exe);
    UE_LOGI("exe fileversion=%u.%u.%u.%u  size=%llu bytes",
            ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF, sizeBytes);
    if constexpr (P::kExpectedExeSize != 0) {
        if (sizeBytes != P::kExpectedExeSize) {
            UE_LOGW("exe size differs from the build these signatures target (%llu) -- "
                    "AOBs/offsets are SUSPECT; re-derive sdk_profile.h for this build.",
                    P::kExpectedExeSize);
        }
    }
}

}  // namespace

void RunHealthCheck() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) {
            UE_LOGI("  [ OK ] %s", what);
        } else {
            UE_LOGE("  [FAIL] %s", what);
            ++fails;
        }
    };

    UE_LOGI("---- SDK health check ----");
    LogGameVersion();

    // 1) AOB resolution. Log each address + RVA, so a bad sig is obvious.
    Resolve();
    uintptr_t base = 0;
    size_t imgSize = 0;
    MainModuleRange(base, imgSize);
    auto rva = [&](uintptr_t a) { return a ? a - base : 0; };
    UE_LOGI("resolve: GUObjectArray=%p (rva 0x%zx)", reinterpret_cast<void*>(g_objArray), rva(g_objArray));
    UE_LOGI("resolve: FName::ToString=%p (rva 0x%zx)",
            reinterpret_cast<void*>(g_fnameToString), rva(reinterpret_cast<uintptr_t>(g_fnameToString)));
    UE_LOGI("resolve: ProcessEvent=%p (rva 0x%zx)",
            reinterpret_cast<void*>(g_processEvent), rva(reinterpret_cast<uintptr_t>(g_processEvent)));
    check(g_objArray != 0, "GUObjectArray signature");
    check(g_fnameToString != nullptr, "FName::ToString signature");
    check(g_processEvent != nullptr, "ProcessEvent signature");

    // The proxy loads before the engine; wait for the object array to populate.
    int32_t n = 0;
    for (int i = 0; i < 120; ++i) {
        n = NumObjects();
        if (n >= 10000) break;
        ::Sleep(500);
    }
    UE_LOGI("NumObjects()=%d", n);
    check(n >= 10000, "object array populated (offsets sane)");

    // 2) Functional validation -- proves the sigs/offsets actually WORK, not
    //    just that an AOB matched something (catches matching the wrong site).
    if (g_objArray && g_fnameToString) {
        // Round-trip a known engine name. Index 1 is the UObject class "Object".
        const std::wstring objName = ToString(NameOf(ObjectAt(1)));
        UE_LOGI("name round-trip: object[1] = '%ls' (expect 'Object')", objName.c_str());
        check(objName == L"Object", "FName::ToString round-trip");

        void* clsObject = FindClass(L"Object");
        void* clsActor = FindClass(P::name::ActorClass);
        void* clsWorld = FindClass(P::name::WorldClass);
        check(clsObject != nullptr, "FindClass(Object)");
        check(clsActor != nullptr, "FindClass(Actor)");
        check(clsWorld != nullptr, "FindClass(World)");
        if (clsActor) {
            void* fn = FindFunction(clsActor, P::name::SetActorLocationFn);
            UE_LOGI("FindFunction(Actor, %ls) = %p", P::name::SetActorLocationFn, fn);
            check(fn != nullptr, "FindFunction(Actor, K2_SetActorLocation)");
        }
    }

    // 3) Gameplay-content signals (informational; absent at the menu).
    void* clsMainPlayer = FindClass(P::name::MainPlayerClass);
    UE_LOGI("content: %ls = %s (loads with the gameplay map; absent at menu)",
            P::name::MainPlayerClass, clsMainPlayer ? "present" : "not loaded");

    if (fails == 0) {
        UE_LOGI("==== HEALTH: PASS ====");
    } else {
        UE_LOGE("==== HEALTH: FAIL (%d issue(s)) -- sdk_profile.h likely needs "
                "re-derivation for this build ====", fails);
    }
}

}  // namespace ue_wrap::reflection
