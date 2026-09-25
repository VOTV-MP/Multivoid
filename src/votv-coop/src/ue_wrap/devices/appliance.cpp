// ue_wrap/devices/appliance.cpp -- see ue_wrap/devices/appliance.h. Per-class engine access for the
// six simple on/off appliances. Offsets and verbs are resolved from the live classes by name; a name
// that does not resolve leaves its class out, since a hard-coded offset would write whatever a newer
// build keeps there.

#include "ue_wrap/devices/appliance.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::appliance {
namespace {

namespace R = reflection;

// One descriptor per appliance class. `applyTakesBool` marks the named setter (serverBox's
// SetActive(bool)); the others direct-write the bool then call a no-arg refresh verb. The bool is the
// one the class's own getData saves: the state its use verb toggles.
struct Desc {
    const wchar_t* className;
    const wchar_t* boolName;
    const wchar_t* applyFn;
    const wchar_t* applyFn2;       // optional 2nd refresh verb (sink: upd() AFTER updIsOn()); nullptr if none
    bool           applyTakesBool;
    // resolved lazily (game-thread serial -- no lock):
    void*   cls;
    int32_t boolOff;
    void*   fn;
    void*   fn2;
    bool    unusable;              // its class loaded but its bool did not resolve: left out, said once
};

// sink_C's BP player_use calls updIsOn() THEN upd() -- updIsOn() flips the tap state, upd()
// repaints the water particle/sound; mirroring only one would leave the FX out of sync. So
// sink carries a 2nd verb. The others' player_use calls a single verb (or SetActive).
//
// The verb is per class, NOT per name: prop_shower_C owns an upd(), but it is the one that
// repaints the DIRT scalar on the cubicle's material, and the tap's own branch instead calls
// updWater() -- the verb that raises the water emitter, the audio component and the actor tick
// off running_cold. Mirroring upd() here wrote the bit with nothing to show for it, which is
// how a shower toggle travelled (applied ok=1 on the far peer) yet ran dry there.
//
// The faucet's is `active`, which its use action toggles before calling upd() and its getData saves;
// its `turnOn` is the look-at flag lookAt sets while a player aims at the tap (faucet_C's bytecode).
Desc g_descs[] = {
    { L"faucet_C",         L"active",       L"upd",       nullptr, false, nullptr, -1, nullptr, nullptr, false },
    { L"sink_C",           L"isOn",         L"updIsOn",   L"upd",  false, nullptr, -1, nullptr, nullptr, false },
    { L"prop_shower_C",    L"running_cold", L"updWater",  nullptr, false, nullptr, -1, nullptr, nullptr, false },
    { L"kitchen_C",        L"Active",       L"upd",       nullptr, false, nullptr, -1, nullptr, nullptr, false },
    { L"serverBox_C",      L"Active",       L"SetActive", nullptr, true,  nullptr, -1, nullptr, nullptr, false },
    { L"wallunit_tapes_C", L"Active",       L"upd",       nullptr, false, nullptr, -1, nullptr, nullptr, false },
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
        const int32_t k = R::FindPropertyOffset(saveCls, L"Key");
        if (k < 0) {
            // No identity, so no appliance syncs: said once, and the family stays off.
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                UE_LOGE("appliance: actor_save_C.Key did not resolve by name -- the appliance family stays off");
            }
            return false;
        }
        g_keyOff = k;
        g_keyResolved.store(true, std::memory_order_release);
        UE_LOGI("appliance: Key@0x%04X (actor_save_C)", k);
    }
    // Lazily resolve each leaf class (best-effort -- cheap hash lookups, skipped once cached).
    bool newlyResolved = false;
    for (auto& d : g_descs) {
        if (d.cls || d.unusable) continue;
        void* cls = R::FindClass(d.className);
        if (!cls) continue;
        const int32_t off = R::FindPropertyOffset(cls, d.boolName);
        if (off < 0) {
            d.unusable = true;
            UE_LOGE("appliance: %ls.%ls did not resolve by name -- this class is left out of the sync",
                    d.className, d.boolName);
            continue;
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
    // The rest: direct-write the bool, then call the no-arg refresh verb (upd/updIsOn) so the mesh,
    // FX and audio repaint from the new state. The toggle itself lives in the BP's player_use; we
    // set the authoritative state and the refresh renders it -- the same rule the lights follow,
    // drive the visual through the verb rather than a bare field write. The channel only applies
    // when cur != want (idempotent guard), so a refresh verb with toggle semantics would also
    // converge.
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
