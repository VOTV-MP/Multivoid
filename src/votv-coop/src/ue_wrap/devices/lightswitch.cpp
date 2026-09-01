// ue_wrap/lightswitch.cpp -- see ue_wrap/lightswitch.h. Engine access for VOTV
// light groups (Atrigger_lightRoot_C). Offsets resolved from the live class via
// reflection (version-portable); the Alpha 0.9.0-n values are logged fallbacks.

#include "ue_wrap/devices/lightswitch.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::lightswitch {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_rootCls    = nullptr;  // trigger_lightRoot_C UClass
int32_t g_keyOff     = -1;       // AtriggerBase_C::Key       (Alpha 0.9.0-n: 0x0260)
int32_t g_isActiveOff = -1;      // trigger_lightRoot_C::IsActive (0x02B8)
void*   g_setActiveFn = nullptr; // SetActive(bool Active) -- writes the GATE only (see the header)
int32_t g_gateOff     = -1;      // trigger_lightRoot_C::active -- the ENABLE GATE (distinct from IsActive)
void*   g_runTriggerFn = nullptr;// runTrigger(owner, index) -- index 1/2 are the ABSOLUTE setters
int32_t g_objectsOff  = -1;      // triggerBase_C::objects (TArray<UObject*>) -- objects[0] is the switch's root
int32_t g_swTriggerOff = -1;     // Alightswitch_C::Trigger -- the legacy single-pointer fallback

constexpr int32_t kKeyOffFallback      = 0x0260;
constexpr int32_t kIsActiveOffFallback = 0x02B8;

// --- The light SWITCH (Alightswitch_C) ---
std::atomic<bool> g_swResolved{false};
void*   g_swCls   = nullptr;  // lightswitch_C UClass
int32_t g_swKeyOff = -1;      // AtriggerBase_C::Key (shared base offset, 0x0260)
int32_t g_swAOff  = -1;       // Alightswitch_C::A (the flip bool, 0x02A0)
void*   g_useFn   = nullptr;  // use()
constexpr int32_t kSwitchAOffFallback = 0x02A0;

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* rootCls = R::FindClass(L"trigger_lightRoot_C");
    if (!rootCls) return false;

    int32_t keyOff = -1;
    if (void* trigCls = R::FindClass(L"triggerBase_C")) {
        keyOff = R::FindPropertyOffset(trigCls, L"Key");
    }
    if (keyOff < 0) {
        UE_LOGW("lightswitch: reflected Key offset not found -- using fallback 0x%04X", kKeyOffFallback);
        keyOff = kKeyOffFallback;
    }
    int32_t isActiveOff = R::FindPropertyOffset(rootCls, L"IsActive");
    if (isActiveOff < 0) {
        UE_LOGW("lightswitch: reflected IsActive offset not found -- using fallback 0x%04X", kIsActiveOffFallback);
        isActiveOff = kIsActiveOffFallback;
    }
    void* setActiveFn = R::FindFunction(rootCls, L"SetActive");
    if (!setActiveFn) {
        UE_LOGW("lightswitch: SetActive UFunction not found -- not ready");
        return false;
    }
    // `active` (the gate) and `IsActive` (the live state) are two SEPARATE FBoolProperties on
    // this class and FName lookup is case-insensitive, so these two names resolve two fields.
    // No fallback offset for the gate on purpose: we WRITE it, and a write through a guessed
    // offset corrupts an unrelated field instead of merely reading a wrong number.
    const int32_t gateOff = R::FindPropertyOffset(rootCls, L"Active");
    if (gateOff < 0) UE_LOGW("lightswitch: reflected `active` gate offset not found -- gate ops disabled");
    void* runTriggerFn = R::FindFunction(rootCls, L"runTrigger");
    if (!runTriggerFn) UE_LOGW("lightswitch: runTrigger UFunction not found -- absolute group apply disabled");
    g_gateOff      = gateOff;
    g_runTriggerFn = runTriggerFn;

    g_rootCls     = rootCls;
    g_keyOff      = keyOff;
    g_isActiveOff = isActiveOff;
    g_setActiveFn = setActiveFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("lightswitch: resolved trigger_lightRoot_C=%p Key@0x%04X IsActive@0x%04X SetActive=%p",
            rootCls, keyOff, isActiveOff, setActiveFn);
    return true;
}

void* SetActiveFn() { return g_setActiveFn; }

bool IsLightRoot(void* obj) {
    if (!obj || !g_rootCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_rootCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* root) {
    if (!root || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(root) + g_keyOff);
    return R::ToString(key);
}

bool TryReadActive(void* root, bool& on) {
    if (!root || g_isActiveOff < 0) return false;
    on = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(root) + g_isActiveOff);
    return true;
}

bool CallSetActive(void* root, bool on) {
    if (!root || !g_setActiveFn) return false;
    ParamFrame f(g_setActiveFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"Active", on);
    return Call(root, f);
}

// --- The light SWITCH (Alightswitch_C) ---

bool EnsureSwitchResolved() {
    if (g_swResolved.load(std::memory_order_acquire)) return true;
    void* cls = R::FindClass(L"lightswitch_C");
    if (!cls) return false;
    int32_t keyOff = -1;
    if (void* trigCls = R::FindClass(L"triggerBase_C")) keyOff = R::FindPropertyOffset(trigCls, L"Key");
    if (keyOff < 0) keyOff = kKeyOffFallback;
    int32_t aOff = R::FindPropertyOffset(cls, L"A");
    if (aOff < 0) {
        UE_LOGW("lightswitch: switch A offset not found -- using fallback 0x%04X", kSwitchAOffFallback);
        aOff = kSwitchAOffFallback;
    }
    void* useFn = R::FindFunction(cls, L"use");
    if (!useFn) { UE_LOGW("lightswitch: switch use() UFunction not found -- not ready"); return false; }
    // The switch reaches its group through the inherited triggerBase_C::objects array (the BP
    // does Array_Get(objects, 0) then casts to the int_Ttrigger interface). `Trigger` is an
    // older single-pointer field kept as a fallback -- the bytecode does not read it.
    if (void* trigCls = R::FindClass(L"triggerBase_C")) g_objectsOff = R::FindPropertyOffset(trigCls, L"objects");
    g_swTriggerOff = R::FindPropertyOffset(cls, L"Trigger");
    g_swCls = cls; g_swKeyOff = keyOff; g_swAOff = aOff; g_useFn = useFn;
    g_swResolved.store(true, std::memory_order_release);
    UE_LOGI("lightswitch: resolved switch lightswitch_C=%p Key@0x%04X A@0x%04X use=%p",
            cls, keyOff, aOff, useFn);
    return true;
}

bool IsLightSwitch(void* obj) {
    if (!obj || !g_swCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_swCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetSwitchKeyString(void* sw) {
    if (!sw || g_swKeyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(sw) + g_swKeyOff);
    return R::ToString(key);
}

bool TryReadSwitchA(void* sw, bool& on) {
    if (!sw || g_swAOff < 0) return false;
    on = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(sw) + g_swAOff);
    return true;
}

bool CallUse(void* sw) {
    if (!sw || !g_useFn) return false;
    ParamFrame f(g_useFn);
    if (!f.valid()) return false;
    return Call(sw, f);
}

// --- The GROUP as a synced entity -----------------------------------------

void* ResolveSwitchRoot(void* sw) {
    if (!sw) return nullptr;
    if (!EnsureResolved()) return nullptr;        // need the lightRoot class to validate what we find
    if (!EnsureSwitchResolved()) return nullptr;  // ...and the SWITCH class, which owns g_objectsOff /
                                                  // g_swTriggerOff. Every caller today happens to have
                                                  // resolved it first; a fourth would silently have got
                                                  // nullptr here, and nullptr means "no gating".
    // objects[0] first -- that is what use() actually reads.
    if (g_objectsOff >= 0) {
        const auto* arr = reinterpret_cast<const field_io::TArrayView*>(
            reinterpret_cast<const char*>(sw) + g_objectsOff);
        if (arr->data && arr->num > 0) {
            void* first = *reinterpret_cast<void* const*>(arr->data);
            if (first && R::IsLive(first) && IsLightRoot(first)) return first;
        }
    }
    if (g_swTriggerOff >= 0) {
        void* t = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(sw) + g_swTriggerOff);
        if (t && R::IsLive(t) && IsLightRoot(t)) return t;
    }
    return nullptr;
}

bool CallRunTrigger(void* root, int32_t index) {
    if (!root || !g_runTriggerFn) return false;
    ParamFrame f(g_runTriggerFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"owner", root);   // never read by the BP on any index path; a valid object beats null
    f.Set<int32_t>(L"index", index);
    return Call(root, f);
}

bool ApplyGroupState(void* root, bool on) { return CallRunTrigger(root, on ? 1 : 2); }

bool GetGroupGate(void* root) {
    if (!root || g_gateOff < 0) return true;  // unknown -> report OPEN, the authored default
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(root) + g_gateOff);
}

void SetGroupGate(void* root, bool open) {
    if (!root || g_gateOff < 0) return;  // fail CLOSED on an unresolved offset: never write a guess
    // The pointer may be an actor of a world that has since been torn down -- the restore half of
    // a hold can outlive the press that took it. A raw write at a known-good offset into freed
    // memory corrupts whatever now owns the page, and by the recorded lesson it faults nowhere
    // near here, so liveness is checked rather than assumed.
    if (!R::IsLive(root)) return;
    *reinterpret_cast<bool*>(reinterpret_cast<char*>(root) + g_gateOff) = open;
}

bool GroupGateAvailable() { return g_gateOff >= 0; }

ScopedGroupGateShut::ScopedGroupGateShut(void* root) {
    if (!root || !GroupGateAvailable()) return;   // guard not in force; shut() reports it
    root_  = root;
    prior_ = GetGroupGate(root);
    SetGroupGate(root, false);
    shut_  = (GetGroupGate(root) == false);       // verify, do not assume the write took
}

ScopedGroupGateShut::~ScopedGroupGateShut() {
    if (root_) SetGroupGate(root_, prior_);
}

}  // namespace ue_wrap::lightswitch
