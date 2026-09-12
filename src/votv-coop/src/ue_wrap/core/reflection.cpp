#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/cached_obj_ref.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sig_scan.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ue_wrap::reflection {
namespace {

// All version-specific knowledge lives in the profile, the porting surface.
namespace P = profile;
namespace O = profile::off;

using FNameToStringFn = void(__fastcall*)(const FName*, FString*);
using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);

uintptr_t g_objArray = 0;
FNameToStringFn g_fnameToString = nullptr;

// The throttled count of IsLive calls that faulted reading a freed pointer. File-scope, so
// the SEH-guarded IsLive holds no local object needing unwinding. A fault means a cached
// UObject* was freed by GC, expected for a stale cache that then re-resolves; a high rate
// flags a lifetime issue.
std::atomic<uint64_t> g_isLiveFaultCount{0};
ProcessEventFn g_processEvent = nullptr;

}  // namespace

// The engine-heap allocator (EngineAlloc, EngineFree and the GMalloc slot) lives in
// ue_wrap/core/engine_heap.cpp; ResolveEngineHeap resolves the slot once, and Resolve calls it
// alongside the other primitives.
void ResolveEngineHeap();

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

// The coop-origin dispatch latch: every dispatch our code issues goes through this one choke
// point, so a thread-local depth tells "the mod called this native" from "the game's own
// blueprint called it" inside an interceptor; the context object alone cannot, since our
// re-arms set timers on game objects. A depth, not a bool, so a nested dispatch stays tagged;
// RAII, so an unwind cannot leave it stuck.
namespace {
thread_local int t_coopDispatchDepth = 0;
struct CoopDispatchScope {
    CoopDispatchScope() { ++t_coopDispatchDepth; }
    ~CoopDispatchScope() { --t_coopDispatchDepth; }
};
}  // namespace

bool InCoopDispatch() { return t_coopDispatchDepth > 0; }

// Coop-call attribution: which reflected calls make up the blueprint dispatches per frame our
// code authors, keyed on the target UFunction, since that is what names the polling (a
// per-tick location poll shows as one function at dozens per frame) with no stack walk.
// Counts only when armed, so shipping pays one relaxed load. Here and not in the ProcessEvent
// detour: this choke point runs at a small fraction of the detour's rate and is our own code
// rather than the engine's hottest path, where an instrument in the unprotected outer frame
// once crashed the game.
namespace {
constexpr int kCallSites = 128;
std::atomic<bool> g_callCensusOn{false};
struct CallSite {
    std::atomic<void*> fn{nullptr};
    std::atomic<unsigned long long> n{0};
};
CallSite g_callSites[kCallSites];

void NoteCoopCall(void* fn) {
    for (int i = 0; i < kCallSites; ++i) {
        void* cur = g_callSites[i].fn.load(std::memory_order_relaxed);
        if (cur == fn) { g_callSites[i].n.fetch_add(1, std::memory_order_relaxed); return; }
        if (!cur) {
            void* expected = nullptr;
            if (g_callSites[i].fn.compare_exchange_strong(expected, fn,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                g_callSites[i].n.fetch_add(1, std::memory_order_relaxed);
            }
            // Whether the slot was won or lost, the next pass re-reads it; losing a race must not
            // drop the sample.
            --i;
        }
    }
}
}  // namespace

void SetCoopCallCensus(bool on) { g_callCensusOn.store(on, std::memory_order_relaxed); }

bool CoopCallSiteAt(int i, void** outFn, unsigned long long* outCount) {
    if (i < 0 || i >= kCallSites || !outFn || !outCount) return false;
    *outFn = g_callSites[i].fn.load(std::memory_order_relaxed);
    *outCount = g_callSites[i].n.load(std::memory_order_relaxed);
    return *outFn != nullptr;
}

bool CallFunction(void* object, void* function, void* params) {
    if (!g_processEvent || !object || !function) return false;
    if (g_callCensusOn.load(std::memory_order_relaxed)) NoteCoopCall(function);
    CoopDispatchScope scope;
    g_processEvent(object, function, params);
    return true;
}

uintptr_t ObjectArrayAddress() { return g_objArray; }

int32_t NumObjects() {
    if (!g_objArray) return 0;
    return *reinterpret_cast<int32_t*>(g_objArray + O::FUObjectArray_ObjObjects + O::Chunk_NumElements);
}

namespace {
// The address of the FUObjectItem (Object, Flags, Cluster, Serial) for a slot, or null when
// the index is out of range or the chunk unallocated.
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
    return item ? *reinterpret_cast<void**>(item) : nullptr;  // the object pointer is the item's first member
}

namespace {
// The slot whose RootSet bit belongs to `obj`, or null if the slot is empty or recycled to
// another object. The identity check matters on the clear path: un-rooting a pointer whose
// slot has been handed to someone else would clear an innocent object's bit.
uint8_t* RootFlagSlotFor(void* obj) {
    if (!obj) return nullptr;
    // SEH around the deref, the shape IsLive uses and for the same reason: this reads the
    // object's own memory, and the one caller that can arrive with a freed pointer is a GcPin
    // destructor at process teardown. A fault there means the object is gone and the un-root is
    // moot: answer no slot rather than die in a CRT terminator under the loader lock.
    int32_t idx;
    __try {
        idx = *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(obj) + O::UObject_InternalIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    uint8_t* item = ItemAt(idx);
    if (!item || *reinterpret_cast<void**>(item) != obj) return nullptr;
    return item;
}
}  // namespace

bool AddToRoot(void* obj) {
    uint8_t* item = RootFlagSlotFor(obj);
    if (!item) return false;
    int32_t& flags = *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags);
    flags |= slot_flags::RootSet;
    return true;
}

bool RemoveFromRoot(void* obj) {
    uint8_t* item = RootFlagSlotFor(obj);
    if (!item) return false;
    int32_t& flags = *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags);
    flags &= ~slot_flags::RootSet;
    return true;
}

int32_t InternalIndexOf(void* obj) {
    if (!obj) return -1;
    // Dereferences obj; the caller guarantees it is mapped (see the header).
    return *reinterpret_cast<int32_t*>(
        reinterpret_cast<uint8_t*>(obj) + O::UObject_InternalIndex);
}

int32_t InternalFlagsOf(void* obj) {
    if (!obj) return 0;
    // Dereferences obj for its index; the caller guarantees it is mapped.
    uint8_t* item = ItemAt(*reinterpret_cast<int32_t*>(
        reinterpret_cast<uint8_t*>(obj) + O::UObject_InternalIndex));
    if (!item || *reinterpret_cast<void**>(item) != obj) return 0;  // slot empty/recycled
    return *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags);
}

bool IsLiveByIndex(void* obj, int32_t internalIdx) {
    if (!obj || internalIdx < 0) return false;
    // Reads only the array slot at the cached index, never the object's possibly freed memory; a
    // purged or recycled slot no longer points back at obj, so the compare fails cleanly.
    uint8_t* item = ItemAt(internalIdx);
    if (!item || *reinterpret_cast<void**>(item) != obj) return false;  // slot empty/recycled
    // The slot check alone is not enough across a level transition: an actor being torn down is
    // flagged PendingKill, then Unreachable, yet occupies its slot until the purge completes, and
    // calling a UFunction on it is a use-after-free. Those flags read as not live (the UE4.27
    // internal flag values, the same guard UE4SS uses).
    const int32_t flags = *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags);
    return (flags & slot_flags::Dying) == 0;
}

void* ResolveWeakObject(int32_t internalIdx, int32_t serial) {
    if (internalIdx < 0 || serial == 0) return nullptr;
    uint8_t* item = ItemAt(internalIdx);
    if (!item) return nullptr;
    if (*reinterpret_cast<int32_t*>(item + O::FUObjectItem_SerialNumber) != serial)
        return nullptr;                       // slot recycled -- a different object lives here
    const int32_t flags = *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags);
    if ((flags & slot_flags::Dying) != 0) return nullptr;
    return *reinterpret_cast<void**>(item);
}

int32_t SlotSerial(int32_t internalIdx) {
    if (internalIdx < 0) return 0;
    uint8_t* item = ItemAt(internalIdx);
    if (!item) return 0;
    return *reinterpret_cast<int32_t*>(item + O::FUObjectItem_SerialNumber);
}

int32_t AllocateSlotSerial(int32_t internalIdx) {
    if (internalIdx < 0) return 0;
    uint8_t* item = ItemAt(internalIdx);
    if (!item) return 0;
    // The engine's FUObjectArray::AllocateSerialNumber, step for step: keep an existing serial, else
    // draw the next from the master counter and install it unless another thread got there first.
    auto* slot = reinterpret_cast<volatile long*>(item + O::FUObjectItem_SerialNumber);
    const long have = *slot;
    if (have != 0) return have;
    const long fresh = ::InterlockedIncrement(
        reinterpret_cast<volatile long*>(g_objArray + O::FUObjectArray_MasterSerialNumber));
    const long was = ::InterlockedCompareExchange(slot, fresh, 0);
    return was != 0 ? was : fresh;
}

int32_t SlotFlags(int32_t internalIdx) {
    if (internalIdx < 0) return 0;
    uint8_t* item = ItemAt(internalIdx);
    return item ? *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags) : 0;
}

namespace {
// The cold path: log one IsLive fault with the caller attributed module-relative, so a fault
// names its call site rather than IsLive itself (a co-resident crash reporter surfaces the
// absorbed fault as a crash naming only IsLive). The first 16 faults log unconditionally, so a
// burst names every site, then one per thousand.
void ReportIsLiveFault(void* obj, void* caller) {
    const uint64_t n = g_isLiveFaultCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 16 && (n % 1000) != 0) return;
    char path[MAX_PATH] = {};
    const char* base = "?";
    uintptr_t off = reinterpret_cast<uintptr_t>(caller);
    HMODULE h = nullptr;
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(caller), &h) &&
        h && ::GetModuleFileNameA(h, path, sizeof(path))) {
        base = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        off -= reinterpret_cast<uintptr_t>(h);
    }
    UE_LOGW("reflection: IsLive caught AV reading freed pointer %p (fault #%llu, caller %s+0x%llX) -- reporting not-live",
            obj, static_cast<unsigned long long>(n), base,
            static_cast<unsigned long long>(off));
}
}  // namespace

bool IsLive(void* obj) {
    if (!obj) return false;
    void* const caller = _ReturnAddress();  // captured at entry: _ReturnAddress is unreliable inside an __except funclet
    // SEH guards the one read of the object's own memory. IsLive is the primitive for checking a
    // cached UObject* that may have been purged (the local player cache, held props, cached
    // singletons): when the purge has freed and unmapped it, the index read faults, and the fault
    // is caught and reported as not live, so the caller's clear-and-rescan path runs instead of
    // crashing and the per-tick re-fault loop ends (a bare read aborted the whole tick task before
    // the caller could clear its cache, and the same dead pointer re-faulted every tick). Faults
    // are surfaced, throttled, never hidden. If the address was reused by a new object the read
    // does not fault, but the slot compare in IsLiveByIndex still rejects the impostor; callers
    // holding many pointers capture the index up front and use IsLiveByIndex directly.
    int32_t idx;
    __try {
        idx = *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(obj) + O::UObject_InternalIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReportIsLiveFault(obj, caller);
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
// Render `name` into the per-thread scratch FString and return its characters and length, or
// null on failure. The buffer is reused across calls: the zero-allocation primitive under
// ToString, NameEquals and NameStartsWith. The pointer is valid until the next render on this
// thread.
const wchar_t* RenderNameToScratch(const FName& name, int& lenOut) {
    lenOut = 0;
    if (!g_fnameToString) return nullptr;
    // Thread-local, so two threads rendering names never race one FString.
    thread_local FString scratch{nullptr, 0, 0};
    // FName::ToString does not reuse the caller's buffer: it drops the data pointer without
    // freeing and allocates a fresh engine buffer every call, so the previous render's buffer is
    // orphaned unless freed here. This is the hot name primitive (every NameEquals, ToString and
    // FindFunction goes through it), and the unfreed orphan was a multi-megabyte-per-second
    // engine-heap leak. A no-op until GMalloc is resolved.
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

// Name comparisons are case-insensitive: engine FNames compare by ComparisonIndex, and the
// rendered string carries whatever casing was registered first in the process, so a
// case-sensitive compare would make by-name lookups depend on package load order (a
// FindFunction once returned null all session because an earlier class had registered the
// name in another case). Two engine names never differ only by case, so insensitive matching
// is strictly more correct.
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
    // A bounded scan: the scratch is not guaranteed null-terminated after the trim, so no wcsstr;
    // needles are a few characters. Case-insensitive, like NameEquals.
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
// The class-name to UClass cache for the by-class walkers. Primed on the first textual match
// during a normal walk; thereafter a walk compares ClassOf against one pointer, with no name
// render. Entries are revalidated once per walk: a blueprint class dies on world unload and
// its address can be recycled, so a cached class must still be live and still carry the
// expected name before a walk trusts it. Keyed by FNV-1a of the text with the full string
// stored for verification; a collision falls back to the slow path. Mutex-guarded, since
// nothing forbids net-thread lookups.
struct CachedClass {
    std::wstring name;
    // A slot-validated reference: the cache is read on any thread, Alive reads only array slots,
    // and the name deref in BeginClassWalk is reached only behind slot validation, so no SEH probe
    // of a possibly freed class and no first-chance fault for a co-resident handler.
    ue_wrap::CachedObjRef cls;
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

// The revalidated cached UClass for the name, or null when unresolved, stale or a hash
// collision. Once per walk, not per object.
void* BeginClassWalk(const wchar_t* className, uint64_t& hashOut) {
    hashOut = Fnv1a(className);
    void* cls = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_classCacheMu);
        auto it = g_classCache.find(hashOut);
        if (it == g_classCache.end()) return nullptr;
        if (::wcscmp(it->second.name.c_str(), className) != 0) return nullptr;  // collision
        cls = it->second.cls.Raw();
    }
    // Revalidated outside the lock, and the order is load-bearing: Alive true means the array slot
    // at the captured index still points at the class (a slot read, valid on any thread), and only
    // then is the unguarded name deref in NameEquals safe; the short-circuit is the guard.
    {
        ue_wrap::CachedObjRef ref;
        {
            std::lock_guard<std::mutex> lk(g_classCacheMu);
            auto it = g_classCache.find(hashOut);
            if (it != g_classCache.end()) ref = it->second.cls;
        }
        void* live = ref.Get();
        if (live && NameEquals(NameOf(live), className)) return live;
    }
    std::lock_guard<std::mutex> lk(g_classCacheMu);
    auto it = g_classCache.find(hashOut);
    if (it != g_classCache.end() && it->second.cls.Raw() == cls) it->second.cls.Reset();
    return nullptr;
}

void PrimeClassWalk(const wchar_t* className, uint64_t hash, void* cls) {
    std::lock_guard<std::mutex> lk(g_classCacheMu);
    auto& e = g_classCache[hash];
    if (e.name.empty()) e.name = className;
    else if (::wcscmp(e.name.c_str(), className) != 0) return;  // collision: leave the first owner
    e.cls.Set(cls);  // fresh from the caller's walk
}

// The per-object match for the walkers. Fast path: a pointer compare against the walk-cached
// class. Slow path: render-compare, and prime the cache on the first hit so the rest of this
// walk and every later walk take the fast path.
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
    // The cache the three sibling walkers have. Without it every call walked the whole object
    // array rendering a name per entry, and hundreds of call sites paid that; the load case was a
    // class resolved per inbound world-actor spawn while the game-thread pump drained dozens of
    // spawns in one frame. BeginClassWalk revalidates the slot and re-compares the name, so this
    // is the siblings' semantics exactly. A miss is deliberately not cached: a class can load
    // later.
    uint64_t hash = 0;
    if (void* cached = BeginClassWalk(className, hash)) return cached;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (!NameEquals(NameOf(obj), className)) continue;
        // Its meta-class identifies it as a class object: every UClass-derived meta-type ends in
        // "Class" (BlueprintGeneratedClass, WidgetBlueprintGeneratedClass, DynamicClass and the
        // rest), so the suffix match resolves blueprint-generated classes too, and the exact name
        // match above already excludes instances, which carry numeric suffixes. Only the handful of
        // name matches pay for the string.
        const std::wstring meta = ClassNameOf(obj);
        if (meta.size() >= 5 && meta.compare(meta.size() - 5, 5, L"Class") == 0) {
            PrimeClassWalk(className, hash, obj);
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
        // Skip the class default object, named Default__<Class>.
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

// The FProperty and FField walkers live in reflection_props.cpp.

namespace {

// Log the running exe's version and size, and warn if it differs from the build the profile
// was derived against; the first thing to check on a new release.
void LogGameVersion() {
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(::GetModuleHandleW(nullptr), exe, MAX_PATH);

    unsigned long long sizeBytes = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(exe, GetFileExInfoStandard, &fad)) {
        sizeBytes = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }

    // The exe file version, which catches engine-version changes that shift offsets.
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

int RunHealthCheck() {
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

    // AOB resolution; each address and RVA is logged, so a bad signature is obvious.
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

    // The mod loads before the engine populates the object array; wait for it.
    int32_t n = 0;
    for (int i = 0; i < 120; ++i) {
        n = NumObjects();
        if (n >= 10000) break;
        ::Sleep(500);
    }
    UE_LOGI("NumObjects()=%d", n);
    check(n >= 10000, "object array populated (offsets sane)");

    // Functional validation: proves the signatures and offsets work, not only that an AOB matched
    // something.
    if (g_objArray && g_fnameToString) {
        // Round-trip a known engine name; index 1 is the UObject class Object.
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

    // Gameplay-content signals, informational; absent at the menu.
    void* clsMainPlayer = FindClass(P::name::MainPlayerClass);
    UE_LOGI("content: %ls = %s (loads with the gameplay map; absent at menu)",
            P::name::MainPlayerClass, clsMainPlayer ? "present" : "not loaded");

    if (fails == 0) {
        UE_LOGI("==== HEALTH: PASS ====");
    } else {
        UE_LOGE("==== HEALTH: FAIL (%d issue(s)) -- sdk_profile.h likely needs "
                "re-derivation for this build ====", fails);
    }
    return fails;
}

}  // namespace ue_wrap::reflection
