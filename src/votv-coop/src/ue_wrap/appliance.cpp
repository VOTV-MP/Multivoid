// ue_wrap/appliance.cpp -- see ue_wrap/appliance.h. Per-class engine access for the six
// simple on/off appliances. Offsets/verbs resolved from the live classes via reflection
// (version-portable); the Alpha 0.9.0-n values are logged fallbacks.

#include "ue_wrap/appliance.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::appliance {
namespace {

namespace R = reflection;

// One descriptor per appliance class. `applyTakesBool` marks the named setter (serverBox's
// SetActive(bool)); the others direct-write the bool then call a no-arg refresh verb.
struct Desc {
    const wchar_t* className;
    const wchar_t* boolName;
    int32_t        boolOffFallback;
    const wchar_t* applyFn;
    const wchar_t* applyFn2;       // optional 2nd refresh verb (sink: upd() AFTER updIsOn()); nullptr if none
    bool           applyTakesBool;
    // resolved lazily (game-thread serial -- no lock):
    void*   cls;
    int32_t boolOff;
    void*   fn;
    void*   fn2;
};

// sink_C's BP player_use calls updIsOn() THEN upd() -- updIsOn() flips the tap state, upd()
// repaints the water particle/sound; mirroring only one would leave the FX out of sync. So
// sink carries a 2nd verb. The others' player_use calls a single upd() (or SetActive).
Desc g_descs[] = {
    { L"faucet_C",         L"turnon",       0x0278, L"upd",       nullptr, false, nullptr, -1, nullptr, nullptr },
    { L"sink_C",           L"isOn",         0x0278, L"updIsOn",   L"upd",  false, nullptr, -1, nullptr, nullptr },
    { L"prop_shower_C",    L"running_cold", 0x0298, L"upd",       nullptr, false, nullptr, -1, nullptr, nullptr },
    { L"kitchen_C",        L"Active",       0x02E1, L"upd",       nullptr, false, nullptr, -1, nullptr, nullptr },
    { L"serverBox_C",      L"Active",       0x03D5, L"SetActive", nullptr, true,  nullptr, -1, nullptr, nullptr },
    { L"wallunit_tapes_C", L"Active",       0x0290, L"upd",       nullptr, false, nullptr, -1, nullptr, nullptr },
};
constexpr int kNumDescs = sizeof(g_descs) / sizeof(g_descs[0]);

std::atomic<bool> g_keyResolved{false};
int32_t g_keyOff = -1;                 // Aactor_save_C::Key (Alpha 0.9.0-n: 0x0230)

void* g_bases[kNumDescs] = {};         // resolved class pointers, for the IsAppliance fast filter
int   g_nBases = 0;

// Find the descriptor whose class matches `obj` -- exact-class pointer compare first (the
// common case: an appliance instance IS its class), then a hierarchy walk for any subclass.
Desc* DescFor(void* obj) {
    if (!obj) return nullptr;
    void* cls = R::ClassOf(obj);
    if (!cls) return nullptr;
    for (auto& d : g_descs)
        if (d.cls && cls == d.cls) return &d;          // exact match (fast path)
    for (auto& d : g_descs) {
        if (!d.cls) continue;
        void* base[1] = { d.cls };
        if (R::IsDescendantOfAny(cls, base, 1)) return &d;  // subclass fallback
    }
    return nullptr;
}

}  // namespace

bool EnsureResolved() {
    // The shared Key lives on the Aactor_save_C base; FindPropertyOffset does NOT climb to a
    // super, so resolve it against actor_save_C directly (same gotcha garage/door handle).
    if (!g_keyResolved.load(std::memory_order_acquire)) {
        void* saveCls = R::FindClass(L"actor_save_C");
        if (!saveCls) return false;  // base not loaded yet
        int32_t k = R::FindPropertyOffset(saveCls, L"Key");
        if (k < 0) {
            UE_LOGW("appliance: reflected Key offset not found -- using fallback 0x0230");
            k = 0x0230;
        }
        g_keyOff = k;
        g_keyResolved.store(true, std::memory_order_release);
        UE_LOGI("appliance: Key@0x%04X (actor_save_C)", k);
    }
    // Lazily resolve each leaf class (best-effort -- cheap hash lookups, skipped once cached).
    bool newlyResolved = false;
    for (auto& d : g_descs) {
        if (d.cls) continue;
        void* cls = R::FindClass(d.className);
        if (!cls) continue;
        int32_t off = R::FindPropertyOffset(cls, d.boolName);
        if (off < 0) {
            UE_LOGW("appliance: %ls.%ls offset not found -- fallback 0x%04X",
                    d.className, d.boolName, d.boolOffFallback);
            off = d.boolOffFallback;
        }
        void* fn = R::FindFunction(cls, d.applyFn);
        if (!fn)
            UE_LOGW("appliance: %ls.%ls() apply verb not found -- field write only",
                    d.className, d.applyFn);
        void* fn2 = nullptr;
        if (d.applyFn2) {
            fn2 = R::FindFunction(cls, d.applyFn2);
            if (!fn2)
                UE_LOGW("appliance: %ls.%ls() 2nd refresh verb not found", d.className, d.applyFn2);
        }
        d.cls = cls;
        d.boolOff = off;
        d.fn = fn;
        d.fn2 = fn2;
        newlyResolved = true;
        UE_LOGI("appliance: resolved %ls bool@0x%04X fn=%p fn2=%p", d.className, off, fn, fn2);
    }
    if (newlyResolved) {
        g_nBases = 0;
        for (auto& d : g_descs)
            if (d.cls) g_bases[g_nBases++] = d.cls;
    }
    return g_keyResolved.load(std::memory_order_acquire);
}

bool IsAppliance(void* obj) {
    if (!obj || g_nBases == 0) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    return R::IsDescendantOfAny(cls, g_bases, g_nBases);
}

std::wstring GetKeyString(void* a) {
    if (!a || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(a) + g_keyOff);
    return R::ToString(key);
}

bool TryReadState(void* a, bool& on) {
    Desc* d = DescFor(a);
    if (!d || d->boolOff < 0) return false;
    on = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(a) + d->boolOff);
    return true;
}

bool ApplyState(void* a, bool on) {
    Desc* d = DescFor(a);
    if (!d) return false;
    if (d->applyTakesBool) {
        // serverBox: SetActive(bNewActive) sets Active AND repaints in one named setter.
        if (!d->fn) {
            if (d->boolOff >= 0) *reinterpret_cast<bool*>(reinterpret_cast<char*>(a) + d->boolOff) = on;
            return false;
        }
        ParamFrame f(d->fn);
        if (!f.valid()) return false;
        f.Set<bool>(L"bNewActive", on);
        return Call(a, f);
    }
    // The rest: direct-write the bool, then call the no-arg refresh verb (upd/updIsOn) so the
    // mesh/FX/audio repaint from the new state. The toggle itself lives in the BP's player_use;
    // we set the authoritative state, the refresh renders it (the lights' "drive the visual via
    // the verb, not a bare field write" lesson). The channel only applies when cur != want
    // (idempotent guard), so a refresh verb with toggle semantics would also converge.
    if (d->boolOff < 0) return false;
    *reinterpret_cast<bool*>(reinterpret_cast<char*>(a) + d->boolOff) = on;
    bool ok = true;
    if (d->fn) {
        ParamFrame f(d->fn);
        if (f.valid()) ok = Call(a, f);
    }
    // sink: upd() AFTER updIsOn() so the water particle/sound repaints too (mirrors the BP's
    // player_use sequence). nullptr/no-op for the single-verb appliances.
    if (d->fn2) {
        ParamFrame f2(d->fn2);
        if (f2.valid()) Call(a, f2);
    }
    return ok;
}

}  // namespace ue_wrap::appliance
