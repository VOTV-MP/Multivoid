// ue_wrap/garage.cpp -- see ue_wrap/garage.h. Engine access for the base garage door
// (Agarage_C). Offsets resolved from the live class via reflection (version-portable);
// the Alpha 0.9.0-n values are logged fallbacks.

#include "ue_wrap/garage.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::garage {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_garageCls = nullptr;  // garage_C UClass
int32_t g_keyOff    = -1;       // AtriggerBase_C::Key (Alpha 0.9.0-n: 0x0260)
int32_t g_openOff   = -1;       // Agarage_C::Open     (0x02E8)
void*   g_acivaeFn  = nullptr;  // acivae() -- the NATIVE animated swing (montage from 0 @0.5x + the
                                // move timeline over its full duration). NOT settime: settime SNAPS
                                // (move.SetNewTime(endpoint) + montage @StartingPosition=100) = the
                                // "too fast" the user saw. Bytecode-verified 2026-06-09.

constexpr int32_t kKeyOffFallback  = 0x0260;
constexpr int32_t kOpenOffFallback = 0x02E8;

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = R::FindClass(L"garage_C");
    if (!cls) return false;

    // Key lives on the AtriggerBase_C base; FindPropertyOffset does NOT climb to the
    // super, so resolve it against triggerBase_C directly (same gotcha door/lightswitch
    // handle).
    int32_t keyOff = -1;
    if (void* trigCls = R::FindClass(L"triggerBase_C")) {
        keyOff = R::FindPropertyOffset(trigCls, L"Key");
    }
    if (keyOff < 0) {
        UE_LOGW("garage: reflected Key offset not found -- using fallback 0x%04X", kKeyOffFallback);
        keyOff = kKeyOffFallback;
    }
    int32_t openOff = R::FindPropertyOffset(cls, L"Open");
    if (openOff < 0) {
        UE_LOGW("garage: reflected Open offset not found -- using fallback 0x%04X", kOpenOffFallback);
        openOff = kOpenOffFallback;
    }
    void* acivaeFn = R::FindFunction(cls, L"acivae");
    if (!acivaeFn) {
        UE_LOGW("garage: acivae UFunction not found -- not ready");
        return false;
    }

    g_garageCls = cls;
    g_keyOff    = keyOff;
    g_openOff   = openOff;
    g_acivaeFn  = acivaeFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("garage: resolved garage_C=%p Key@0x%04X Open@0x%04X acivae=%p",
            cls, keyOff, openOff, acivaeFn);
    return true;
}

bool IsGarage(void* obj) {
    if (!obj || !g_garageCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_garageCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* g) {
    if (!g || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(g) + g_keyOff);
    return R::ToString(key);
}

bool TryReadOpen(void* g, bool& open) {
    if (!g || g_openOff < 0) return false;
    open = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(g) + g_openOff);
    return true;
}

bool ApplyOpen(void* g, bool open) {
    if (!g || !g_acivaeFn) return false;
    // Idempotent: if already in the target state, do nothing (skip the re-trigger + the echo).
    bool cur = false;
    if (TryReadOpen(g, cur) && cur == open) return true;
    // Two bytecode-verified facts (RE 2026-06-09) drive this:
    //  (1) Neither settime() NOR acivae() writes the `Open` bool @0x02E8 -- the ONLY writers are
    //      runTrigger's E-press toggle and the game's own loadTriggerData (`open := value; settime`).
    //      So we MUST set the field ourselves, else the mirror's poll baseline goes stale and the
    //      symmetric Channel re-broadcasts the opposite -> the open/close OSCILLATION the user first saw.
    //  (2) settime() SNAPS (move.SetNewTime(endpoint) + a montage at StartingPosition=100) -> the
    //      "garage slides too fast on the host" the user saw next; acivae() ANIMATES (montage from 0
    //      @0.5x + move.Play/Reverse over the full ~10s, DIRECTION read from the `Open` field).
    // So: write Open := target FIRST (fixes the oscillation + gives acivae its direction), THEN call
    // acivae() for the NATIVE animated swing (fixes the too-fast). acivae has no `mov` guard, so a
    // mid-swing opposite packet just re-aims it (last-writer-wins). This is exactly the code path a
    // local E-press takes (runTrigger toggles Open -> acivae), minus the toggle.
    if (g_openOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(g) + g_openOff) = open;
    ParamFrame f(g_acivaeFn);  // acivae() takes no params -- it reads the Open field for direction
    if (!f.valid()) return false;
    return Call(g, f);
}

}  // namespace ue_wrap::garage
