// ue_wrap/devices/garage.cpp -- see ue_wrap/devices/garage.h. Engine access for the base garage
// door (Agarage_C). Offsets resolved from the live class via reflection (version-portable); the
// Alpha 0.9.0-n values are logged fallbacks.

#include "ue_wrap/devices/garage.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::garage {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_garageCls = nullptr;  // garage_C UClass
int32_t g_openOff   = -1;       // Agarage_C::Open     (0x02E8)
void*   g_acivaeFn  = nullptr;  // acivae() -- the NATIVE animated swing (the montage from position
                                // 0 at half rate, and the move timeline over its full length). NOT
                                // settime, which runs the same timeline and then SNAPS it: the
                                // montage at full rate from position 100, then move.SetNewTime
                                // straight to the endpoint.
// No Key offset: identity is the level-export FName (GetNameKey), not the save key -- see
// garage.h for why the key cannot serve as one.

constexpr int32_t kOpenOffFallback = 0x02E8;

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = R::FindClass(L"garage_C");
    if (!cls) return false;

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
    g_openOff   = openOff;
    g_acivaeFn  = acivaeFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("garage: resolved garage_C=%p Open@0x%04X acivae=%p (identity=level-export FName)",
            cls, openOff, acivaeFn);
    return true;
}

bool IsGarage(void* obj) {
    if (!obj || !g_garageCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_garageCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetNameKey(void* g) {
    // Identity is the garage's level-export FName, baked into the cooked package and so
    // deterministic and cross-peer stable, NOT the save key. Mirrors ue_wrap::door_box::GetNameKey,
    // the proven author for keyless placed actors; garage.h says why the save key is unreliable.
    if (!g) return std::wstring();
    return R::ToString(R::NameOf(g));
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
    // Two facts about the blueprint drive this:
    //   (1) Neither settime() nor acivae() writes the `Open` bool. The only writers
    //       are runTrigger's E-press toggle and the game's own loadTriggerData
    //       (`open := value; settime`). So we must set the field ourselves, or the
    //       mirror's poll baseline goes stale and the symmetric Channel re-broadcasts
    //       the opposite -- an open/close oscillation.
    //   (2) settime() SNAPS: it plays the timeline, then puts the montage at position
    //       100 at full rate and calls move.SetNewTime(endpoint). acivae() ANIMATES:
    //       the montage from 0 at half rate and move.Play/Reverse over the full
    //       timeline, taking its DIRECTION from the `Open` field.
    // So: write Open := target FIRST -- that both fixes the oscillation and gives acivae its
    // direction -- THEN call acivae() for the native animated swing. acivae itself has no `mov`
    // guard; the guard sits in runTrigger, which ignores an E-press mid-swing. So a mid-swing
    // opposite packet just re-aims the door, last writer wins. This is the path a local E-press
    // takes (runTrigger toggles Open, then calls acivae), minus the toggle and minus that guard.
    if (g_openOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(g) + g_openOff) = open;
    ParamFrame f(g_acivaeFn);  // acivae() takes no params -- it reads the Open field for direction
    if (!f.valid()) return false;
    return Call(g, f);
}

}  // namespace ue_wrap::garage
