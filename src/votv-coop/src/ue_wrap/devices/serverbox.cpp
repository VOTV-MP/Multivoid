// ue_wrap/devices/serverbox.cpp -- see ue_wrap/devices/serverbox.h.

#include "ue_wrap/devices/serverbox.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <chrono>

namespace ue_wrap::serverbox {
namespace {

namespace R = reflection;
namespace P = profile;

using field_io::TArrayView;
using field_io::ReadFStringAt;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Every group latches after this many passes that had the class in hand and still came up short:
// a member missing from a loaded class does not appear later, so the retry is only worth the
// window where the class itself is still loading, and a pass that keeps warning is a warning once
// a second forever.
constexpr int kMaxPostClassAttempts = 5;

int32_t g_offName   = -1;
void*   g_fnProcess = nullptr;  // pocessFloppy
void*   g_fnEject   = nullptr;  // ejectFloppy
bool     g_resolved = false;
bool     g_verbsLatchedOff = false;
int      g_verbAttempts = 0;
uint64_t g_nextTryMs = 0;

// The box list resolves on its own: the break-and-fix lane wants the boxes and nothing else, so a
// slot field this wrapper cannot find must not cost it the list.
int32_t  g_offServers = -1;  // mainGamemode_C.servers
uint64_t g_nextServersTryMs = 0;

bool EnsureServersResolved() {
    if (g_offServers >= 0) return true;
    const uint64_t now = NowMs();
    if (now < g_nextServersTryMs) return false;
    g_nextServersTryMs = now + 1000;
    void* gmCls = R::FindClass(P::name::GamemodeClass);
    if (!gmCls) return false;
    g_offServers = R::FindPropertyOffset(gmCls, L"servers");
    if (g_offServers < 0) return false;
    UE_LOGI("serverbox: server list resolved (servers=0x%X)", g_offServers);
    return true;
}

// The live gamemode, held through the slot-validated cache (a recycled slot, a dead world and a
// reused serial all read as not-alive), and the generation a caller anchors a baseline to.
CachedObjRef g_gm;
uint32_t     g_gmGeneration = 0;

void* Gamemode() {
    if (g_gm.Alive()) return g_gm.Raw();
    void* found = R::FindObjectByClass(P::name::GamemodeClass);
    // A re-resolve that lands on the same object is the cache being re-primed, not a new world:
    // only a different one is a generation the baselines above us must not carry across.
    const bool changed = (found != g_gm.Raw());
    g_gm.Set(found);
    if (found && changed) ++g_gmGeneration;
    return g_gm.Raw();
}

// ---- break state: the gamemode's three totals, the box's flag, and its re-skin ----------------

int32_t g_offBroken   = -1;  // mainGamemode_C.brokenServers
int32_t g_offEffCalc  = -1;  // serverEfficiency_calc
int32_t g_offEffDownl = -1;  // serverEfficiency_downl
int32_t g_offIsBroken = -1;  // serverBox_C.IsBroken, byte offset
uint8_t g_maskIsBroken = 0;  // ...and its FBoolProperty real bit
void*   g_fnCheck = nullptr; // serverBox_C::check()
bool     g_breakResolved = false;
bool     g_breakLatchedOff = false;
int      g_breakAttempts = 0;
uint64_t g_nextBreakTryMs = 0;

bool BreakGroupComplete() {
    return g_offBroken >= 0 && g_offEffCalc >= 0 && g_offEffDownl >= 0 && g_offIsBroken >= 0 &&
           g_maskIsBroken != 0 && g_fnCheck != nullptr;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    if (g_verbsLatchedOff) return false;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;

    void* cls = R::FindClass(L"serverBox_C");
    if (!cls) return false;  // world not loaded yet

    g_offName   = R::FindPropertyOffset(cls, L"name");
    g_fnProcess = R::FindFunction(cls, L"pocessFloppy");
    g_fnEject   = R::FindFunction(cls, L"ejectFloppy");

    // No offset fallbacks: an unresolved member here means the class is not the one this wrapper
    // was written against, and a guessed offset would write into whatever now lives there.
    if (g_offName < 0 || !g_fnProcess || !g_fnEject) {
        if (++g_verbAttempts >= kMaxPostClassAttempts) {
            g_verbsLatchedOff = true;
            UE_LOGW("serverbox: resolution incomplete after %d passes (name=%d pocessFloppy=%p "
                    "ejectFloppy=%p) -- the box's verbs stay off; game version mismatch?",
                    g_verbAttempts, g_offName, g_fnProcess, g_fnEject);
        }
        return false;
    }
    g_resolved = true;
    UE_LOGI("serverbox: resolved (name=0x%X insert=%p eject=%p)", g_offName, g_fnProcess,
            g_fnEject);
    return true;
}

size_t ReadServers(std::vector<void*>& out) {
    if (!EnsureServersResolved()) return 0;
    void* gm = Gamemode();
    if (!gm) return 0;
    const auto* arr = reinterpret_cast<const TArrayView*>(
        reinterpret_cast<const uint8_t*>(gm) + g_offServers);
    if (!arr->data || arr->num <= 0) return 0;
    void* const* elems = reinterpret_cast<void* const*>(arr->data);
    const size_t before = out.size();
    for (int32_t i = 0; i < arr->num; ++i) out.push_back(elems[i]);
    return out.size() - before;
}

std::wstring ReadName(void* box) {
    if (!box || !g_resolved) return std::wstring();
    return ReadFStringAt(box, g_offName);
}

bool CallProcessFloppy(void* box, void* discActor) {
    if (!box || !discActor || !g_resolved) return false;
    ParamFrame f(g_fnProcess);
    if (!f.valid()) return false;
    if (!f.Set(L"Object", discActor)) return false;
    return Call(box, f);
}

bool CallEjectFloppy(void* box) {
    if (!box || !g_resolved) return false;
    ParamFrame f(g_fnEject);
    if (!f.valid()) return false;
    return Call(box, f);
}

bool EnsureBreakResolved() {
    if (g_breakResolved) return true;
    if (g_breakLatchedOff) return false;
    const uint64_t now = NowMs();
    if (now < g_nextBreakTryMs) return false;
    g_nextBreakTryMs = now + 2000;

    void* gmCls = R::FindClass(P::name::GamemodeClass);
    void* sbCls = R::FindClass(L"serverBox_C");
    if (!gmCls || !sbCls) return false;  // world not loaded yet

    if (g_offBroken   < 0) g_offBroken   = R::FindPropertyOffset(gmCls, L"brokenServers");
    if (g_offEffCalc  < 0) g_offEffCalc  = R::FindPropertyOffset(gmCls, L"serverEfficiency_calc");
    if (g_offEffDownl < 0) g_offEffDownl = R::FindPropertyOffset(gmCls, L"serverEfficiency_downl");
    if (g_offIsBroken < 0) R::FindBoolProperty(sbCls, L"IsBroken", g_offIsBroken, g_maskIsBroken);
    if (!g_fnCheck) g_fnCheck = R::FindFunction(sbCls, L"check");

    if (BreakGroupComplete()) {
        g_breakResolved = true;
        UE_LOGI("serverbox: break state resolved (broken=0x%X eff=0x%X/0x%X IsBroken=0x%X "
                "mask=0x%02X check=yes)", g_offBroken, g_offEffCalc, g_offEffDownl, g_offIsBroken,
                g_maskIsBroken);
        return true;
    }
    if (++g_breakAttempts >= kMaxPostClassAttempts) {
        g_breakLatchedOff = true;
        UE_LOGW("serverbox: break state INCOMPLETE after %d passes (broken=0x%X IsBroken=0x%X "
                "check=%s) -- latched off; game version mismatch?", g_breakAttempts, g_offBroken,
                g_offIsBroken, g_fnCheck ? "yes" : "no");
    }
    return false;
}

bool ReadIsBroken(void* box) {
    if (!box || !g_breakResolved) return false;
    const uint8_t b = *(reinterpret_cast<const uint8_t*>(box) + g_offIsBroken);
    return (b & g_maskIsBroken) != 0;
}

bool ApplyBreak(void* box, bool broken) {
    if (!box || !g_breakResolved) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(box) + g_offIsBroken;
    if (broken) *p |= g_maskIsBroken;
    else        *p &= static_cast<uint8_t>(~g_maskIsBroken);
    ParamFrame f(g_fnCheck);
    if (!f.valid()) return false;
    return Call(box, f);
}

bool ReadAggregates(Aggregates& out) {
    if (!g_breakResolved) return false;
    void* gm = Gamemode();
    if (!gm) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(gm);
    out.brokenServers      = *reinterpret_cast<const int32_t*>(base + g_offBroken);
    out.efficiencyCalc     = *reinterpret_cast<const float*>  (base + g_offEffCalc);
    out.efficiencyDownload = *reinterpret_cast<const float*>  (base + g_offEffDownl);
    return true;
}

bool WriteAggregates(const Aggregates& in) {
    if (!g_breakResolved) return false;
    void* gm = Gamemode();
    if (!gm) return false;
    auto* base = reinterpret_cast<uint8_t*>(gm);
    *reinterpret_cast<int32_t*>(base + g_offBroken)   = in.brokenServers;
    *reinterpret_cast<float*>  (base + g_offEffCalc)  = in.efficiencyCalc;
    *reinterpret_cast<float*>  (base + g_offEffDownl) = in.efficiencyDownload;
    return true;
}

uint32_t GamemodeGeneration() {
    Gamemode();  // a stale cache is revalidated here, so the generation a caller reads is current
    return g_gmGeneration;
}

}  // namespace ue_wrap::serverbox
