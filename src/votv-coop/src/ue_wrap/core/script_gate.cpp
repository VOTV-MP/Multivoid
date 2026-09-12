// ue_wrap/core/script_gate.cpp -- see ue_wrap/core/script_gate.h.
//
// The loop is derived, not pattern-scanned: the exec-handler table (GNatives) is resolved by
// its dispatch-site signature and validated, the local-final and local-virtual handlers are read
// out of it, and each is scanned for the rip-relative address it hands ProcessScriptFunction as
// the body executor; the two must agree, and the candidate's own first bytes must hold the
// loop's return-opcode compare and a reference back to the same table. Four facts that agree,
// or no install.

#include "ue_wrap/core/script_gate.h"

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sig_scan.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace ue_wrap::script_gate {
namespace {

namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;

// The loop's ABI, shared with the exec handlers: (Context, FFrame&, Result). The return is a
// leftover register no caller reads; forwarded unchanged.
using LoopFn = std::uintptr_t(__fastcall*)(void* ctx, void* stack, void* result);

constexpr int kOpcodeLocalVirtual = 0x45;
constexpr int kOpcodeLocalFinal   = 0x46;
constexpr std::uint8_t kExReturn  = 0x04;
constexpr std::uint8_t kExNothing = 0x0B;

LoopFn g_trampoline = nullptr;
void*  g_target = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_countOn{false};

// Fixed open-addressing tables, never rehashed and never freed: a slot's key is published last
// with release, so the loop's reader sees an empty slot or a whole entry, and a retired watch
// only clears its enabled flag, leaving the probe chain intact. Two key spaces, two tables: an
// exact watch keys on the UFunction pointer, a name watch on the FName's two indices (its
// unresolved placeholder holds a second slot). What fills a table is its KEYED slots, retired
// ones included, so the cap counts those: at most 96 of 256, a miss probes under two slots.
constexpr int kSlots = 256;
constexpr int kMaxKeyed = 96;

struct Entry {
    std::atomic<std::uint64_t> key{0};
    std::atomic<bool> enabled{false};
    int tag = 0;
    PreFn pre = nullptr;
    PostFn post = nullptr;
    const wchar_t* name = nullptr;   // a name watch's registered literal; null for an exact one
    std::atomic<bool> resolved{true}; // a name watch is inert until its FName is known
};
Entry g_fnTable[kSlots];
Entry g_nameTable[kSlots];
std::atomic<int> g_fnWatches{0};      // enabled exact watches
std::atomic<int> g_nameWatches{0};    // enabled name watches, placeholders included
std::atomic<int> g_namesPending{0};
int g_fnKeyed = 0;                    // keyed slots, under the registration mutex
int g_nameKeyed = 0;
std::mutex g_regMutex;   // registration only; never on the call path

inline std::uint64_t HashKey(std::uint64_t key) {
    return (key >> 4) * 0x9E3779B97F4A7C15ull;
}
inline int SlotOf(std::uint64_t key) {
    return static_cast<int>(HashKey(key) >> 56) & (kSlots - 1);
}
inline std::uint64_t NameKey(const R::FName& n) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(n.ComparisonIndex)) << 32) |
           static_cast<std::uint32_t>(n.Number);
}

// Counters: relaxed atomics, torn reads tolerated by the readers.
std::atomic<unsigned long long> g_calls{0};
std::atomic<unsigned long long> g_callsGT{0};
std::atomic<unsigned long long> g_matched{0};
std::atomic<unsigned long long> g_cancelled{0};
std::atomic<unsigned long long> g_offGT{0};
std::atomic<unsigned long long> g_faults{0};

// The ambient window, per thread, published only around a watched body.
thread_local int            t_depth = 0;
thread_local int            t_tag = 0;
thread_local void*          t_object = nullptr;
thread_local void*          t_function = nullptr;
thread_local const wchar_t* t_name = nullptr;

struct ActiveScope {
    int prevTag; void* prevObject; void* prevFunction; const wchar_t* prevName;
    ActiveScope(int tag, void* object, void* function, const wchar_t* name)
        : prevTag(t_tag), prevObject(t_object), prevFunction(t_function), prevName(t_name) {
        ++t_depth; t_tag = tag; t_object = object; t_function = function; t_name = name;
    }
    ~ActiveScope() {
        --t_depth; t_tag = prevTag; t_object = prevObject; t_function = prevFunction; t_name = prevName;
    }
    ActiveScope(const ActiveScope&) = delete;
    ActiveScope& operator=(const ActiveScope&) = delete;
};

template <class T> inline T Read(const void* base, size_t off) {
    T v; std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(base) + off, sizeof(T)); return v;
}

// ---- the crash firewall around a consumer callback -----------------------------------------
// SEH only in these two frames (no C++ unwind may share a frame with __try). A stack overflow is
// passed on: the guard page is gone and absorbing would run the engine on an exhausted stack.
int FaultFilter(EXCEPTION_POINTERS* ep, void** ip) {
    if (ep->ExceptionRecord->ExceptionCode == static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW))
        return EXCEPTION_CONTINUE_SEARCH;
    *ip = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
int RunPreSEH(PreFn cb, const Call& call, Verdict* out, void** ip) {
    __try { *out = cb(call); return 0; }
    __except (FaultFilter(GetExceptionInformation(), ip)) { return 1; }
}
int RunPostSEH(PostFn cb, const Call& call, void** ip) {
    __try { cb(call); return 0; }
    __except (FaultFilter(GetExceptionInformation(), ip)) { return 1; }
}
// A fault names its callback and its site; the first few per phase print in full.
void LogFault(const char* phase, const Call& call, void* ip) {
    g_faults.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> s_printed{0};
    if (s_printed.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    const std::wstring fn = call.function ? R::ToString(R::NameOf(call.function)) : L"<null>";
    UE_LOGE("script_gate: %s callback FAULT absorbed -- function='%ls' object=%p tag=%d ip=%p; "
            "the body runs as if the callback had answered Run", phase, fn.c_str(),
            call.object, call.tag, ip);
}

// ---- the call path ----------------------------------------------------------------------------
// Every matching entry of a table fires; the first Cancel wins. Returns true when cancelled.
bool FireTable(Entry* table, std::uint64_t key, const Call& base, bool post) {
    bool cancel = false;
    for (int i = SlotOf(key), n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
        const std::uint64_t k = table[i].key.load(std::memory_order_acquire);
        if (k == 0) break;
        if (k != key) continue;
        Entry& e = table[i];
        if (!e.enabled.load(std::memory_order_acquire) || !e.resolved.load(std::memory_order_acquire)) continue;
        Call call = base;
        call.tag = e.tag;
        void* ip = nullptr;
        if (post) {
            if (e.post && RunPostSEH(e.post, call, &ip) != 0) LogFault("post", call, ip);
        } else if (e.pre) {
            Verdict v = Verdict::Run;
            if (RunPreSEH(e.pre, call, &v, &ip) != 0) { LogFault("pre", call, ip); v = Verdict::Run; }
            if (v == Verdict::Cancel) cancel = true;
        }
    }
    return cancel;
}

// The innermost entry's identity for the ambient window: the first enabled match in either table.
const Entry* FirstMatch(Entry* table, std::uint64_t key) {
    for (int i = SlotOf(key), n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
        const std::uint64_t k = table[i].key.load(std::memory_order_acquire);
        if (k == 0) return nullptr;
        if (k == key && table[i].enabled.load(std::memory_order_acquire) &&
            table[i].resolved.load(std::memory_order_acquire))
            return &table[i];
    }
    return nullptr;
}

std::uintptr_t __fastcall LoopDetour(void* ctx, void* stack, void* result) {
    // The tax every script call pays for the life of the process: one relaxed load and a
    // predicted branch while disabled; enabled, one hashed probe per key space.
    if (!g_enabled.load(std::memory_order_relaxed)) return g_trampoline(ctx, stack, result);
    if (g_countOn.load(std::memory_order_relaxed)) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        if (GT::IsGameThread()) g_callsGT.fetch_add(1, std::memory_order_relaxed);
    }
    void* fn = Read<void*>(stack, P::off::FFrame_Node);
    const std::uint64_t fnKey = reinterpret_cast<std::uintptr_t>(fn);
    const Entry* hit = g_fnWatches.load(std::memory_order_relaxed) > 0 ? FirstMatch(g_fnTable, fnKey) : nullptr;
    std::uint64_t nameKey = 0;
    if (g_nameWatches.load(std::memory_order_relaxed) > 0) {
        nameKey = NameKey(R::NameOf(fn));
        if (!hit) hit = FirstMatch(g_nameTable, nameKey);
    }
    if (!hit) return g_trampoline(ctx, stack, result);

    if (!GT::IsGameThread()) {
        // The consumers reach the engine and our reflection, both game-thread only.
        g_offGT.fetch_add(1, std::memory_order_relaxed);
        return g_trampoline(ctx, stack, result);
    }
    g_matched.fetch_add(1, std::memory_order_relaxed);

    Call call{};
    call.object = Read<void*>(stack, P::off::FFrame_Object);
    call.function = fn;
    call.locals = Read<std::uint8_t*>(stack, P::off::FFrame_Locals);
    call.result = result;
    if (void* prev = Read<void*>(stack, P::off::FFrame_PreviousFrame)) {
        call.callerObject = Read<void*>(prev, P::off::FFrame_Object);
        call.callerFunction = Read<void*>(prev, P::off::FFrame_Node);
    }
    call.stack = stack;
    call.depth = t_depth + 1;
    call.fromOurCode = R::InCoopDispatch();

    bool cancel = FireTable(g_fnTable, fnKey, call, /*post=*/false);
    if (nameKey) cancel = FireTable(g_nameTable, nameKey, call, /*post=*/false) || cancel;
    if (cancel) {
        g_cancelled.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    std::uintptr_t rv;
    {
        ActiveScope scope(hit->tag, call.object, fn, hit->name);
        rv = g_trampoline(ctx, stack, result);
    }
    FireTable(g_fnTable, fnKey, call, /*post=*/true);
    if (nameKey) FireTable(g_nameTable, nameKey, call, /*post=*/true);
    return rv;
}

// ---- deriving the loop ---------------------------------------------------------------------
std::uintptr_t* ResolveGNatives() {
    // The dispatch site `lea r9,[GNatives]; ... movzx; ... call [r9+rax*8]`: the rel32 sits at
    // hit+3, so the table is hit + 7 + rel32.
    const uintptr_t hit = ue_wrap::FindPattern(
        "4C 8D 0D ?? ?? ?? ?? 49 8B D7 0F B6 08 48 FF C0 49 89 47 20 8B C1 49 8B 4F 18 41 FF 14 C1");
    if (!hit) return nullptr;
    const std::int32_t rel = *reinterpret_cast<std::int32_t*>(hit + 3);
    return reinterpret_cast<std::uintptr_t*>(hit + 7 + rel);
}

bool InModule(uintptr_t p, uintptr_t base, size_t size) { return p >= base && p < base + size; }

bool ValidateTable(const std::uintptr_t* tbl, uintptr_t base, size_t size) {
    int inRange = 0;
    for (int i = 0; i < 256; ++i)
        if (InModule(tbl[i], base, size)) ++inRange;
    return inRange >= 200;
}

// A `lea r64,[rip+disp32]` at p (REX.W with or without REX.R; ModRM mod=00 rm=101) -> its target.
bool DecodeLeaRip(const std::uint8_t* p, uintptr_t& out) {
    if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) return false;
    std::int32_t rel; std::memcpy(&rel, p + 3, sizeof(rel));
    out = reinterpret_cast<uintptr_t>(p) + 7 + rel;
    return true;
}

// The loop's shape: within its body, the return-opcode compare, the nothing-opcode compare (the
// last thing it does, past the loop itself) and a rip-relative reference to the exec-handler
// table. The window covers the whole function; a short one once refused the real loop.
bool LooksLikeLoop(uintptr_t t, uintptr_t gnatives, uintptr_t base, size_t size) {
    constexpr size_t kWindow = 0xA0;
    if (!InModule(t, base, size) || !InModule(t + kWindow, base, size)) return false;
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(t);
    bool cmpReturn = false, cmpNothing = false, namesTable = false;
    for (size_t i = 0; i + 7 <= kWindow; ++i) {
        if (p[i] == 0x80 && p[i + 1] == 0x38 && p[i + 2] == kExReturn) cmpReturn = true;
        if (p[i] == 0x80 && p[i + 1] == 0x38 && p[i + 2] == kExNothing) cmpNothing = true;
        uintptr_t target;
        if (DecodeLeaRip(p + i, target) && target == gnatives) namesTable = true;
    }
    return cmpReturn && cmpNothing && namesTable;
}

// The body executor a handler hands ProcessScriptFunction: the one rip-relative lea in the
// handler whose target has the loop's shape. 0 when there is none or more than one.
uintptr_t ExecutorOfHandler(uintptr_t handler, uintptr_t gnatives, uintptr_t base, size_t size) {
    constexpr size_t kWindow = 0xA0;   // both handlers are under 0xA0 bytes
    if (!InModule(handler, base, size) || !InModule(handler + kWindow, base, size)) return 0;
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(handler);
    uintptr_t found = 0;
    for (size_t i = 0; i + 7 <= kWindow; ++i) {
        uintptr_t target;
        if (!DecodeLeaRip(p + i, target)) continue;
        if (!LooksLikeLoop(target, gnatives, base, size)) continue;
        if (found && found != target) return 0;
        found = target;
    }
    return found;
}

}  // namespace

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    uintptr_t base = 0; size_t size = 0;
    ue_wrap::MainModuleRange(base, size);
    std::uintptr_t* gnatives = ResolveGNatives();
    if (!gnatives || !ValidateTable(gnatives, base, size)) {
        UE_LOGE("script_gate: the exec-handler table did not resolve or validate (%p) -- NOT installed",
                static_cast<void*>(gnatives));
        return false;
    }
    const uintptr_t fromVirtual = ExecutorOfHandler(gnatives[kOpcodeLocalVirtual],
                                                    reinterpret_cast<uintptr_t>(gnatives), base, size);
    const uintptr_t fromFinal = ExecutorOfHandler(gnatives[kOpcodeLocalFinal],
                                                  reinterpret_cast<uintptr_t>(gnatives), base, size);
    if (!fromVirtual || fromVirtual != fromFinal) {
        UE_LOGE("script_gate: the two handlers disagree on the body loop (local-virtual -> exe+0x%llX, "
                "local-final -> exe+0x%llX) -- NOT installed",
                static_cast<unsigned long long>(fromVirtual ? fromVirtual - base : 0),
                static_cast<unsigned long long>(fromFinal ? fromFinal - base : 0));
        return false;
    }
    if (!hook::Init()) return false;
    void* target = reinterpret_cast<void*>(fromVirtual);
    // The loop is a function UE4SS's own PolyHook detours for its Lua script hooks, so the relay
    // must be followJmp-immune, as ProcessEvent's is.
    if (!hook::Install(target, reinterpret_cast<void*>(&LoopDetour),
                       reinterpret_cast<void**>(&g_trampoline), /*followJmpImmune=*/true)) {
        return false;
    }
    g_target = target;
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("script_gate: installed on the VM's script loop at exe+0x%llX (both exec handlers name "
            "it, and it names the exec-handler table at %p); disabled until a session enables it",
            static_cast<unsigned long long>(fromVirtual - base), static_cast<void*>(gnatives));
    return true;
}

bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }

namespace {

// Registration under the mutex: an idempotent re-register re-enables the same slot; a new pair
// takes the first empty slot of the probe chain. Returns false when the chain is full.
bool Register(Entry* table, std::atomic<int>& count, int& keyed, std::uint64_t key, int tag,
              PreFn pre, PostFn post, const wchar_t* name, bool resolved) {
    for (int i = SlotOf(key), n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
        Entry& e = table[i];
        const std::uint64_t k = e.key.load(std::memory_order_relaxed);
        if (k == key && e.tag == tag && e.pre == pre && e.post == post && e.name == name) {
            if (!e.enabled.exchange(true, std::memory_order_release)) count.fetch_add(1, std::memory_order_release);
            return true;
        }
        if (k != 0) continue;
        if (keyed >= kMaxKeyed) return false;
        e.tag = tag; e.pre = pre; e.post = post; e.name = name;
        e.resolved.store(resolved, std::memory_order_relaxed);
        e.enabled.store(true, std::memory_order_relaxed);
        e.key.store(key, std::memory_order_release);   // published last: the reader sees a whole entry
        ++keyed;
        count.fetch_add(1, std::memory_order_release);
        return true;
    }
    return false;
}

bool Retire(Entry* table, std::atomic<int>& count, std::uint64_t key, int tag, PreFn pre,
            PostFn post, const wchar_t* name) {
    for (int i = SlotOf(key), n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
        Entry& e = table[i];
        const std::uint64_t k = e.key.load(std::memory_order_relaxed);
        if (k == 0) return false;
        if (k == key && e.tag == tag && e.pre == pre && e.post == post && e.name == name) {
            if (e.enabled.exchange(false, std::memory_order_release)) count.fetch_sub(1, std::memory_order_release);
            return true;
        }
    }
    return false;
}

}  // namespace

// A watch on a gate that never installed is refused, so a consumer's "live" line cannot print
// over a seam that sees nothing; said once, since the consumers retry from their ticks.
bool RefusedUninstalled(const char* what) {
    if (g_installed.load(std::memory_order_acquire)) return false;
    static std::atomic<bool> s_said{false};
    if (!s_said.exchange(true, std::memory_order_relaxed))
        UE_LOGE("script_gate: %s refused -- the gate is not installed, so no watch can fire", what);
    return true;
}

bool Watch(void* ufunction, int tag, PreFn pre, PostFn post) {
    if (!ufunction || (!pre && !post)) return false;
    if (RefusedUninstalled("Watch")) return false;
    if (Read<std::uint32_t>(ufunction, P::off::UFunction_FunctionFlags) & P::off::FUNC_Native) {
        UE_LOGE("script_gate: %p is a native function; it never runs through the script loop -- refused",
                ufunction);
        return false;
    }
    if (Read<std::int32_t>(ufunction, P::off::UStruct_ScriptNum) <= 0)
        UE_LOGW("script_gate: %p has no bytecode; the watch can never fire", ufunction);
    std::lock_guard<std::mutex> lk(g_regMutex);
    const bool ok = Register(g_fnTable, g_fnWatches, g_fnKeyed, reinterpret_cast<std::uintptr_t>(ufunction),
                             tag, pre, post, nullptr, /*resolved=*/true);
    if (!ok) UE_LOGE("script_gate: watch table full (%d keyed slots) -- cannot watch %p", kMaxKeyed, ufunction);
    return ok;
}

bool Unwatch(void* ufunction, int tag, PreFn pre, PostFn post) {
    if (!ufunction) return false;
    std::lock_guard<std::mutex> lk(g_regMutex);
    return Retire(g_fnTable, g_fnWatches, reinterpret_cast<std::uintptr_t>(ufunction), tag, pre, post, nullptr);
}

bool WatchName(const wchar_t* name, int tag, PreFn pre, PostFn post) {
    if (!name || !*name || (!pre && !post)) return false;
    if (RefusedUninstalled("WatchName")) return false;
    std::lock_guard<std::mutex> lk(g_regMutex);
    // A name's key is its FName, unknown until the game thread converts it; until then the entry
    // is keyed on the literal's address and marked unresolved, and the resolve re-keys it.
    for (int i = 0; i < kSlots; ++i) {
        Entry& e = g_nameTable[i];
        if (e.key.load(std::memory_order_relaxed) != 0 && e.name && e.tag == tag && e.pre == pre &&
            e.post == post && std::wcscmp(e.name, name) == 0) {
            if (!e.enabled.exchange(true, std::memory_order_release)) g_nameWatches.fetch_add(1, std::memory_order_release);
            return true;
        }
    }
    const bool ok = Register(g_nameTable, g_nameWatches, g_nameKeyed, reinterpret_cast<std::uintptr_t>(name) | 1u,
                             tag, pre, post, name, /*resolved=*/false);
    if (!ok) {
        static std::atomic<bool> s_saidFull{false};
        if (!s_saidFull.exchange(true, std::memory_order_relaxed))
            UE_LOGE("script_gate: name watch table full (%d keyed slots) -- cannot watch %ls", kMaxKeyed, name);
        return false;
    }
    g_namesPending.fetch_add(1, std::memory_order_release);
    UE_LOGI("script_gate: watching '%ls' tag=%d -- pending the game-thread name resolve", name, tag);
    GT::Post([] { ResolvePendingNames(); });
    return true;
}

void ResolvePendingNames() {
    if (g_namesPending.load(std::memory_order_acquire) == 0 || !GT::IsGameThread()) return;
    // The string-to-name conversion dispatches ProcessEvent, so it runs OUTSIDE the registration
    // mutex: a registration reached from inside that dispatch would otherwise wait on itself.
    // Under the mutex only the pending literals are collected, and the re-key is done after.
    const wchar_t* names[kSlots];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_regMutex);
        for (int i = 0; i < kSlots && n < kSlots; ++i) {
            const Entry& e = g_nameTable[i];
            if (e.key.load(std::memory_order_relaxed) != 0 && !e.resolved.load(std::memory_order_relaxed) && e.name)
                names[n++] = e.name;
        }
    }
    for (int j = 0; j < n; ++j) {
        const R::FName f = ue_wrap::fname_utils::StringToFName(names[j]);
        if (f.ComparisonIndex == 0) continue;   // not yet; the next tick retries
        std::lock_guard<std::mutex> lk(g_regMutex);
        // An unresolved entry sits at the slot of its placeholder key; once the FName is known
        // it moves to the slot of its real key, and the placeholder slot is left disabled, keyed
        // (its chain stays walkable) and nameless, so no registration matches it again.
        for (int i = 0; i < kSlots; ++i) {
            Entry& e = g_nameTable[i];
            if (e.key.load(std::memory_order_relaxed) == 0 || e.resolved.load(std::memory_order_relaxed) || e.name != names[j]) continue;
            const bool wasEnabled = e.enabled.exchange(false, std::memory_order_release);
            e.name = nullptr;
            e.resolved.store(true, std::memory_order_release);
            if (wasEnabled) g_nameWatches.fetch_sub(1, std::memory_order_release);
            if (Register(g_nameTable, g_nameWatches, g_nameKeyed, NameKey(f), e.tag, e.pre, e.post, names[j], /*resolved=*/true)) {
                UE_LOGI("script_gate: name '%ls' resolved (cmp=0x%x number=0x%x) -- the watch is live",
                        names[j], f.ComparisonIndex, f.Number);
            } else {
                UE_LOGE("script_gate: name '%ls' resolved but the table is full -- the watch is dead", names[j]);
            }
            g_namesPending.fetch_sub(1, std::memory_order_release);
        }
    }
}

void SetEnabled(bool on) {
    const bool was = g_enabled.exchange(on, std::memory_order_release);
    if (was != on) UE_LOGI("script_gate: %s", on ? "ENABLED (session active)" : "DISABLED (session ended)");
}

bool IsEnabled() { return g_enabled.load(std::memory_order_acquire); }

Active CurrentThreadCall() {
    Active a{};
    a.active = t_depth > 0;
    a.tag = t_tag;
    a.depth = t_depth;
    a.object = t_object;
    a.function = t_function;
    a.name = t_name;
    return a;
}

std::uint8_t* OutParamPtr(const Call& call, int32_t paramOffset) {
    if (!call.stack || paramOffset < 0) return nullptr;
    for (void* rec = Read<void*>(call.stack, P::off::FFrame_OutParms); rec;
         rec = Read<void*>(rec, P::off::FOutParmRec_Next)) {
        void* prop = Read<void*>(rec, P::off::FOutParmRec_Property);
        if (prop && Read<std::int32_t>(prop, P::off::FProperty_Offset_Internal) == paramOffset)
            return Read<std::uint8_t*>(rec, P::off::FOutParmRec_PropAddr);
    }
    return nullptr;
}

Stats GetStats() {
    Stats s{};
    s.calls = g_calls.load(std::memory_order_relaxed);
    s.callsGameThread = g_callsGT.load(std::memory_order_relaxed);
    s.matched = g_matched.load(std::memory_order_relaxed);
    s.cancelled = g_cancelled.load(std::memory_order_relaxed);
    s.offGameThread = g_offGT.load(std::memory_order_relaxed);
    s.faults = g_faults.load(std::memory_order_relaxed);
    s.watches = g_fnWatches.load(std::memory_order_relaxed);
    // Live name watches: the enabled count less the placeholders still waiting for their name.
    s.nameWatches = g_nameWatches.load(std::memory_order_relaxed) - g_namesPending.load(std::memory_order_relaxed);
    s.enabled = g_enabled.load(std::memory_order_relaxed);
    s.installed = g_installed.load(std::memory_order_relaxed);
    return s;
}

void SetPerfCounting(bool on) { g_countOn.store(on, std::memory_order_relaxed); }

}  // namespace ue_wrap::script_gate
