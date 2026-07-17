// coop/dev/heap_probe.cpp -- see header. Static-CRT malloc detour leak attribution.

#include "coop/dev/heap_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace coop::dev::heap_probe {
namespace {

using Clock = std::chrono::steady_clock;

// ---- original (un-detoured) CRT entry points, set by MH_CreateHook ----
using malloc_t  = void*(__cdecl*)(size_t);
using free_t    = void (__cdecl*)(void*);
using realloc_t = void*(__cdecl*)(void*, size_t);
using calloc_t  = void*(__cdecl*)(size_t, size_t);
malloc_t  g_origMalloc  = nullptr;
free_t    g_origFree    = nullptr;
realloc_t g_origRealloc = nullptr;
calloc_t  g_origCalloc  = nullptr;

// ---- our DLL's address range (allocation-site attribution filter) ----
uintptr_t g_modBase = 0;
uintptr_t g_modEnd  = 0;

void ResolveModuleRange() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&ResolveModuleRange), &self);
    if (!self) return;
    g_modBase = reinterpret_cast<uintptr_t>(self);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_modBase);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(g_modBase + dos->e_lfanew);
    g_modEnd  = g_modBase + nt->OptionalHeader.SizeOfImage;
}

// ---- a raw allocator that routes THROUGH the saved trampoline, so the probe's
// own bookkeeping containers never re-enter the detoured malloc (no recursion,
// no TLS guard needed). g_origMalloc is set before any detour goes live.
template <class T>
struct RawAlloc {
    using value_type = T;
    RawAlloc() = default;
    template <class U> RawAlloc(const RawAlloc<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = g_origMalloc ? g_origMalloc(n * sizeof(T)) : std::malloc(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept {
        if (g_origFree) g_origFree(p); else std::free(p);
    }
    template <class U> bool operator==(const RawAlloc<U>&) const noexcept { return true; }
    template <class U> bool operator!=(const RawAlloc<U>&) const noexcept { return false; }
};

constexpr int kFrames = 6;  // module frames kept per site (the call chain to read)

struct Info { size_t size; uint64_t site; };
struct Site { int64_t live; int64_t allocs; void* frames[kFrames]; };

template <class K, class V>
using RawMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>,
                                  RawAlloc<std::pair<const K, V>>>;

std::mutex g_mu;
RawMap<void*, Info>    g_ptrs;   // live pointer -> {size, site-hash}
RawMap<uint64_t, Site> g_sites;  // site-hash -> {live bytes, alloc count, frames}
int64_t g_totalLive = 0;

// ---- arming + throttle ----
int  g_armed     = -1;  // -1 unresolved, 0 off, 1 on
bool g_installed = false;
bool g_haveBaseline = false;
Clock::time_point g_next{};
constexpr auto kInterval = std::chrono::seconds(4);

bool Armed() {
    if (g_armed < 0) g_armed = coop::config::IsIniKeyTrue("heap_probe") ? 1 : 0;
    return g_armed == 1;
}

// Capture the call chain INSIDE our module (the static CRT's operator-new/malloc
// wrappers are linked into us too, so the first frames are CRT plumbing and the
// deeper ones are the real caller -- we keep up to kFrames module frames and log
// them all as RVAs so the chain is readable). Returns a stable hash of those
// frames (0 if the allocation has no frame in our module -- impossible for our
// static malloc, but guarded). RtlCaptureStackBackTrace does not call malloc.
uint64_t CaptureSite(void* out[kFrames]) {
    void* fr[28];
    const USHORT n = ::RtlCaptureStackBackTrace(2 /*OurCaller + the Hk_ thunk*/, 28, fr, nullptr);
    int k = 0;
    for (USHORT i = 0; i < n && k < kFrames; ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
        if (a >= g_modBase && a < g_modEnd) out[k++] = fr[i];
    }
    if (k == 0) return 0;
    for (int i = k; i < kFrames; ++i) out[i] = nullptr;
    uint64_t h = 1469598103934665603ULL;  // FNV-1a over the kept frames
    for (int i = 0; i < kFrames; ++i) {
        h ^= reinterpret_cast<uintptr_t>(out[i]);
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

void Record(void* p, size_t n) {
    if (!p) return;
    void* frames[kFrames];
    const uint64_t site = CaptureSite(frames);
    if (!site) return;  // not attributable to our module
    std::lock_guard<std::mutex> lk(g_mu);
    g_ptrs[p] = Info{n, site};
    auto it = g_sites.find(site);
    if (it == g_sites.end()) {
        Site s{};
        s.live = static_cast<int64_t>(n);
        s.allocs = 1;
        for (int i = 0; i < kFrames; ++i) s.frames[i] = frames[i];
        g_sites.emplace(site, s);
    } else {
        it->second.live += static_cast<int64_t>(n);
        it->second.allocs += 1;
    }
    g_totalLive += static_cast<int64_t>(n);
}

void Unrecord(void* p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ptrs.find(p);
    if (it == g_ptrs.end()) return;  // not ours (or allocated before arming)
    auto si = g_sites.find(it->second.site);
    if (si != g_sites.end()) si->second.live -= static_cast<int64_t>(it->second.size);
    g_totalLive -= static_cast<int64_t>(it->second.size);
    g_ptrs.erase(it);
}

void* __cdecl Hk_malloc(size_t n) {
    void* p = g_origMalloc(n);
    Record(p, n);
    return p;
}
void __cdecl Hk_free(void* p) {
    Unrecord(p);
    g_origFree(p);
}
void* __cdecl Hk_realloc(void* p, size_t n) {
    Unrecord(p);
    void* q = g_origRealloc(p, n);
    Record(q, n);
    return q;
}
void* __cdecl Hk_calloc(size_t count, size_t size) {
    void* p = g_origCalloc(count, size);
    Record(p, count * size);
    return p;
}

void InstallHooks() {
    g_installed = true;  // latch first: a failed install must not retry every tick
    ResolveModuleRange();
    if (!g_modBase) { UE_LOGE("[heap_probe] our module range unresolved -- disabled"); g_armed = 0; return; }

    // Our DLL links the static CRT (/MT), so malloc/free live INSIDE this module;
    // &malloc resolves to that statically-linked copy. No other module calls it,
    // so the detour fires for exactly our-module CRT allocations.
    void* pMalloc  = reinterpret_cast<void*>(static_cast<malloc_t>(&std::malloc));
    void* pFree    = reinterpret_cast<void*>(static_cast<free_t>(&std::free));
    void* pRealloc = reinterpret_cast<void*>(static_cast<realloc_t>(&std::realloc));
    void* pCalloc  = reinterpret_cast<void*>(static_cast<calloc_t>(&std::calloc));

    // malloc FIRST so g_origMalloc (used by RawAlloc) is live before any record;
    // free LAST so g_origFree is ready for the first Unrecord.
    bool ok = true;
    ok &= ue_wrap::hook::Install(pMalloc,  reinterpret_cast<void*>(&Hk_malloc),  reinterpret_cast<void**>(&g_origMalloc));
    ok &= ue_wrap::hook::Install(pRealloc, reinterpret_cast<void*>(&Hk_realloc), reinterpret_cast<void**>(&g_origRealloc));
    ok &= ue_wrap::hook::Install(pCalloc,  reinterpret_cast<void*>(&Hk_calloc),  reinterpret_cast<void**>(&g_origCalloc));
    ok &= ue_wrap::hook::Install(pFree,    reinterpret_cast<void*>(&Hk_free),    reinterpret_cast<void**>(&g_origFree));
    if (!ok) UE_LOGE("[heap_probe] one or more static-CRT hooks failed to install");
    UE_LOGW("[heap_probe] hooked static-CRT malloc/free/realloc/calloc; our module [%p, %p) -- "
            "reporting OUR-module CRT live bytes every %llds (engine allocator is NOT counted)",
            reinterpret_cast<void*>(g_modBase), reinterpret_cast<void*>(g_modEnd),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kInterval).count()));
}

void Report() {
    // Snapshot under the lock into a trampoline-backed vector (RawAlloc), then
    // sort + log outside -- none of this re-enters the detour.
    std::vector<std::pair<uint64_t, Site>, RawAlloc<std::pair<uint64_t, Site>>> sites;
    int64_t total = 0;
    size_t liveCount = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        total = g_totalLive;
        liveCount = g_ptrs.size();
        sites.reserve(g_sites.size());
        for (const auto& kv : g_sites)
            if (kv.second.live > 0) sites.push_back({kv.first, kv.second});
    }
    std::sort(sites.begin(), sites.end(),
              [](const auto& a, const auto& b) { return a.second.live > b.second.live; });

    UE_LOGW("[heap_probe] OUR-module CRT live=%.2f MB across %zu live ptr(s), %zu site(s) "
            "-- if this climbs with RSS the top site is the leak; FLAT while RSS climbs => "
            "engine-allocator leak (escalate)",
            static_cast<double>(total) / (1024.0 * 1024.0), liveCount, sites.size());
    const size_t kTop = 12;
    for (size_t i = 0; i < sites.size() && i < kTop; ++i) {
        const Site& s = sites[i].second;
        // Render the module frames as RVAs: resolve via build/votv-coop/Release/votv-coop.map.
        char chain[160];
        int off = 0;
        for (int f = 0; f < kFrames && s.frames[f]; ++f) {
            const uintptr_t rva = reinterpret_cast<uintptr_t>(s.frames[f]) - g_modBase;
            off += _snprintf_s(chain + off, sizeof(chain) - off, _TRUNCATE,
                               "%s+0x%llX", f ? " <- " : "", static_cast<unsigned long long>(rva));
            if (off >= static_cast<int>(sizeof(chain)) - 16) break;
        }
        UE_LOGW("[heap_probe]   live=%9.3f MB  allocs=%-9lld  %s",
                static_cast<double>(s.live) / (1024.0 * 1024.0),
                static_cast<long long>(s.allocs), chain);
    }
}

}  // namespace

void Tick() {
    if (!Armed()) return;
    if (!g_installed) InstallHooks();
    if (g_armed != 1) return;  // install may have disarmed on failure

    const auto now = Clock::now();
    if (g_haveBaseline && now < g_next) return;
    g_next = now + kInterval;
    if (!g_haveBaseline) { g_haveBaseline = true; return; }  // let the first interval accrue
    Report();
}

}  // namespace coop::dev::heap_probe
