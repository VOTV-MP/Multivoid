// ue_wrap/devices/generator.cpp -- see ue_wrap/devices/generator.h. Offsets are the class's layout, the same in
// every world, and resolve once by name; verbs resolve on the instance in hand through the dispatch cache.

#include "ue_wrap/devices/generator.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"            // SpawnActor
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace ue_wrap::generator {
namespace {

namespace R = reflection;
namespace P = profile;

bool     g_resolved = false;
bool     g_refused = false;       // the class loaded but a field did not: permanent for this build
uint64_t g_nextTryMs = 0;
int32_t  g_offList = -1;          // mainGamemode_C.generators
int32_t  g_offBroken = -1;
uint8_t  g_maskBroken = 0;
int32_t  g_offCyc = -1;
uint8_t  g_maskCyc = 0;
int32_t  g_offCycle = -1;
int32_t  g_offUpgrade = -1;
int32_t  g_offButton = -1;        // button_activate
int32_t  g_offTrigger = -1;       // triggerWhenCompleted
int32_t  g_offUpgradeRoot = -1;   // upgradeRoot
int32_t  g_offLookButton = -1;    // lookAtButton, the drill's press
uint8_t  g_maskLookButton = 0;
int32_t  g_offLookUpgrade = -1;   // lookAtUpgrade, the drill's install
uint8_t  g_maskLookUpgrade = 0;
int32_t  g_offUpgradeButtons = -1;  // upgradeButtons, the drill's install
int32_t  g_offUpgradeMeshes = -1;   // upgradeMeshes, the drill's read of what updUpgrades built
ue_wrap::CachedObjRef g_upgradeCls;  // prop_transformerUpgrade_C
int32_t  g_offPanelObj = -1;      // panelObj, the drill's puzzle shortcut
int32_t  g_offTurnOn = -1;        // turnon, the drill's read of the last cue

constexpr const wchar_t* kVerbs[] = { L"break", L"fullFix", L"damage", L"update", L"updUpgrades" };

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool ReadBit(void* obj, int32_t off, uint8_t mask) {
    return (*(reinterpret_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

void WriteBit(void* obj, int32_t off, uint8_t mask, bool v) {
    uint8_t& b = *(reinterpret_cast<uint8_t*>(obj) + off);
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

int32_t& IntAt(void* gen, int32_t off) { return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(gen) + off); }

void* ObjectAt(void* obj, int32_t off) {
    return off < 0 ? nullptr : *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(obj) + off);
}

bool CallVerb(void* gen, const wchar_t* verb) {
    return gen && g_resolved && component_calls::CallParamlessNamed(gen, verb);
}

// The Activate route's last step: its `triggerWhenCompleted`, when set and a trigger, run as runTrigger(gen, 0).
// False when there is none.
bool RunCompletionTrigger(void* gen) {
    void* trigger = ObjectAt(gen, g_offTrigger);
    if (!trigger || !R::IsLive(trigger)) return false;
    // The Activate route casts it to the trigger interface and calls runTrigger(this, 0); an actor without the
    // verb is not a trigger, and the route skips it the same way.
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(trigger), L"runTrigger");
    ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"owner", gen) && f.Set<int32_t>(L"index", 0) && Call(trigger, f);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    if (g_refused) return false;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;
    void* cls = object_index::ClassByName(L"generator_C");
    void* gmCls = object_index::ClassByName(P::name::GamemodeClass);
    if (!cls || !gmCls) return false;  // not streamed in yet; retried a second later

    // Every offset and verb resolves BY NAME; a miss on a loaded class refuses the wrapper for the process.
    auto refuse = [&](const wchar_t* what) {
        g_refused = true;
        UE_LOGE("generator: %ls did not resolve on a loaded generator_C -- the grid's generator rows are OFF for "
                "this game build", what);
        return false;
    };
    const int32_t offList = R::FindPropertyOffset(gmCls, L"generators");
    if (offList < 0) return refuse(L"mainGamemode_C::generators");
    int32_t offBroken = -1, offCyc = -1;
    uint8_t maskBroken = 0, maskCyc = 0;
    if (!R::FindBoolProperty(cls, L"isBroken", offBroken, maskBroken)) return refuse(L"isBroken");
    if (!R::FindBoolProperty(cls, L"cyc", offCyc, maskCyc)) return refuse(L"cyc");
    const int32_t offCycle = R::FindPropertyOffset(cls, L"cycle");
    if (offCycle < 0) return refuse(L"cycle");
    const int32_t offUpgrade = R::FindPropertyOffset(cls, L"upgradeLevel");
    if (offUpgrade < 0) return refuse(L"upgradeLevel");
    const int32_t offButton = R::FindPropertyOffset(cls, L"button_activate");
    if (offButton < 0) return refuse(L"button_activate");
    const int32_t offTrigger = R::FindPropertyOffset(cls, L"triggerWhenCompleted");
    if (offTrigger < 0) return refuse(L"triggerWhenCompleted");
    const int32_t offUpgradeRoot = R::FindPropertyOffset(cls, L"upgradeRoot");
    if (offUpgradeRoot < 0) return refuse(L"upgradeRoot");
    for (const wchar_t* verb : kVerbs)
        if (!R::FindDispatchFunctionCached(cls, verb)) return refuse(verb);
    // The drill's reads only: a miss leaves its press, its shortcut or its cue unavailable, not the lane.
    R::FindBoolProperty(cls, L"lookAtButton", g_offLookButton, g_maskLookButton);
    R::FindBoolProperty(cls, L"lookAtUpgrade", g_offLookUpgrade, g_maskLookUpgrade);
    g_offUpgradeButtons = R::FindPropertyOffset(cls, L"upgradeButtons");
    g_offUpgradeMeshes = R::FindPropertyOffset(cls, L"upgradeMeshes");
    g_offPanelObj = R::FindPropertyOffset(cls, L"panelObj");
    g_offTurnOn = R::FindPropertyOffset(cls, L"turnon");

    g_offList = offList;
    g_offBroken = offBroken;  g_maskBroken = maskBroken;
    g_offCyc = offCyc;        g_maskCyc = maskCyc;
    g_offCycle = offCycle;
    g_offUpgrade = offUpgrade;
    g_offButton = offButton;
    g_offTrigger = offTrigger;
    g_offUpgradeRoot = offUpgradeRoot;
    g_resolved = true;
    UE_LOGI("generator: resolved list@0x%X isBroken@0x%X/%02X cyc@0x%X/%02X cycle@0x%X upgradeLevel@0x%X "
            "button_activate@0x%X triggerWhenCompleted@0x%X upgradeRoot@0x%X", offList, offBroken, maskBroken, offCyc,
            maskCyc, offCycle, offUpgrade, offButton, offTrigger, offUpgradeRoot);
    return true;
}

namespace {

// gamemode.generators in place, or null before the game mode resolves or when the list reads implausible.
const field_io::TArrayView* List() {
    if (!EnsureResolved()) return nullptr;
    void* gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    const auto* arr =
        reinterpret_cast<const field_io::TArrayView*>(reinterpret_cast<const uint8_t*>(gm) + g_offList);
    return (arr->data && arr->num > 0 && arr->num <= 64) ? arr : nullptr;
}

}  // namespace

size_t ReadGenerators(std::vector<void*>& out) {
    const field_io::TArrayView* arr = List();
    if (!arr) return 0;
    void* const* elems = reinterpret_cast<void* const*>(arr->data);
    const size_t before = out.size();
    for (int32_t i = 0; i < arr->num; ++i)  // positions kept: a peer names a generator by its place in the list
        out.push_back((elems[i] && R::IsLive(elems[i])) ? elems[i] : nullptr);
    return out.size() - before;
}

// Read in place: it runs on every client install, press and hit, and on every changed frame of a drag.
int32_t IndexOf(void* gen) {
    const field_io::TArrayView* arr = gen && R::IsLive(gen) ? List() : nullptr;
    if (!arr) return -1;
    void* const* elems = reinterpret_cast<void* const*>(arr->data);
    for (int32_t i = 0; i < arr->num; ++i)
        if (elems[i] == gen) return i;
    return -1;
}

bool ReadRow(void* gen, Row& out) {
    if (!gen || !g_resolved) return false;
    out.broken = ReadBit(gen, g_offBroken, g_maskBroken);
    out.cyc = ReadBit(gen, g_offCyc, g_maskCyc);
    out.cycle = IntAt(gen, g_offCycle);
    out.upgradeLevel = IntAt(gen, g_offUpgrade);
    return true;
}

bool WriteBroken(void* gen, bool broken) {
    if (!gen || !g_resolved) return false;
    WriteBit(gen, g_offBroken, g_maskBroken, broken);
    return true;
}

bool WriteCycle(void* gen, int32_t cycle) {
    if (!gen || !g_resolved) return false;
    IntAt(gen, g_offCycle) = cycle;
    return true;
}

bool WriteCyc(void* gen, bool cyc) {
    if (!gen || !g_resolved) return false;
    WriteBit(gen, g_offCyc, g_maskCyc, cyc);
    return true;
}

bool WriteUpgradeLevel(void* gen, int32_t level) {
    if (!gen || !g_resolved) return false;
    IntAt(gen, g_offUpgrade) = level;
    return true;
}

bool CallBreak(void* gen) { return CallVerb(gen, L"break"); }
bool CallDamage(void* gen) { return CallVerb(gen, L"damage"); }
bool CallUpdUpgrades(void* gen) { return CallVerb(gen, L"updUpgrades"); }

bool Repair(void* gen) {
    if (!CallVerb(gen, L"fullFix") || !CallVerb(gen, L"update")) return false;
    RunCompletionTrigger(gen);
    return true;
}

void* ActivateButton(void* gen) {
    if (!gen || !g_resolved) return nullptr;
    void* button = ObjectAt(gen, g_offButton);
    return (button && R::IsLive(button)) ? button : nullptr;
}

void* UpgradeSlot(void* gen) {
    if (!gen || !g_resolved) return nullptr;
    void* slot = ObjectAt(gen, g_offUpgradeRoot);
    return (slot && R::IsLive(slot)) ? slot : nullptr;
}

bool IsUpgrade(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    if (!g_upgradeCls.Alive()) {
        void* found = object_index::ClassByName(L"prop_transformerUpgrade_C");
        if (!found) return false;  // not loaded: no upgrade exists yet
        g_upgradeCls.Set(found);
    }
    void* base = g_upgradeCls.Raw();
    return R::IsDescendantOfAny(R::ClassOf(actor), &base, 1, 8);
}

void* SpawnUpgrade(const FVector& at) {
    void* cls = object_index::ClassByName(L"prop_transformerUpgrade_C");
    return cls ? engine::SpawnActor(cls, at) : nullptr;
}

bool PressActivate(void* gen, void* player) {
    void* button = ActivateButton(gen);
    void* fn = gen ? R::FindDispatchFunctionCached(R::ClassOf(gen), L"actionOptionIndex") : nullptr;
    if (!button || !player || !fn || g_offLookButton < 0) return false;
    WriteBit(gen, g_offLookButton, g_maskLookButton, true);  // what getActionOptions writes for the button
    const FVector at = engine::GetComponentLocation(button);
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", gen, button, at) &&
           f.Set<uint8_t>(L"action", 4) && f.Set<void*>(L"lookAtComponent", button) && Call(gen, f);
}

bool InsertUpgrade(void* gen, void* player, void* upgrade) {
    void* fn = gen ? R::FindDispatchFunctionCached(R::ClassOf(gen), L"playerUsedOn") : nullptr;
    if (!fn || !player || !upgrade || g_offLookUpgrade < 0 || g_offUpgradeButtons < 0) return false;
    const auto* buttons =
        reinterpret_cast<const field_io::TArrayView*>(reinterpret_cast<const uint8_t*>(gen) + g_offUpgradeButtons);
    void* button = (buttons->data && buttons->num > 0) ? *reinterpret_cast<void* const*>(buttons->data) : nullptr;
    if (!button || !R::IsLive(button)) return false;
    WriteBit(gen, g_offLookUpgrade, g_maskLookUpgrade, true);  // what getActionOptions writes for an upgrade button
    const FVector at = engine::GetComponentLocation(button);
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", gen, button, at) &&
           f.Set<void*>(L"lookAtComponent", button) && f.Set<void*>(L"holdObject", upgrade) && Call(gen, f);
}

int UpgradesNear(const FVector& at, float radius, void** nearest) {
    struct Near { FVector at; float radius; int count; float best; void* nearest; } n{at, radius, 0, radius, nullptr};
    if (nearest) *nearest = nullptr;
    void* cls = object_index::ClassByName(L"prop_transformerUpgrade_C");
    if (!cls) return 0;
    object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t index) {
        // An index member may still be loading, under construction or dying; the slot's flags say so.
        if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
        if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        auto* x = static_cast<Near*>(c);
        FVector p{};
        if (!engine::TryGetActorLocation(obj, p)) return;
        const float d = std::sqrt((p.X - x->at.X) * (p.X - x->at.X) + (p.Y - x->at.Y) * (p.Y - x->at.Y) +
                                  (p.Z - x->at.Z) * (p.Z - x->at.Z));
        if (d > x->radius) return;
        ++x->count;
        if (d <= x->best) {
            x->best = d;
            x->nearest = obj;
        }
    }, &n);
    if (nearest) *nearest = n.nearest;
    return n.count;
}

int UpgradeMeshCount(void* gen) {
    if (!gen || !g_resolved || g_offUpgradeMeshes < 0) return -1;
    const auto* arr =
        reinterpret_cast<const field_io::TArrayView*>(reinterpret_cast<const uint8_t*>(gen) + g_offUpgradeMeshes);
    if (arr->num < 0 || arr->num > 64 || (arr->num > 0 && !arr->data)) return -1;
    void* const* elems = reinterpret_cast<void* const*>(arr->data);
    int live = 0;
    for (int32_t i = 0; i < arr->num; ++i)
        if (elems[i] && R::IsLive(elems[i])) ++live;
    return live;
}

bool WritePuzzleSolved(void* gen) {
    if (!gen || !g_resolved) return false;
    void* panel = ObjectAt(gen, g_offPanelObj);
    if (!panel || !R::IsLive(panel)) return false;
    for (const wchar_t* name : {L"isRotatorsComplete", L"isSineComplete", L"isSwitchesComplete"}) {
        int32_t off = -1;
        uint8_t mask = 0;
        if (!R::FindBoolProperty(R::ClassOf(panel), name, off, mask)) return false;
        WriteBit(panel, off, mask, true);
    }
    return true;
}

bool ReadLastCue(void* gen, bool& turnOn) {
    if (!gen || !g_resolved) return false;
    void* comp = ObjectAt(gen, g_offTurnOn);
    if (!comp || !R::IsLive(comp)) return false;
    void* sound = ObjectAt(comp, R::FindPropertyOffset(R::ClassOf(comp), L"Sound"));
    if (!sound || !R::IsLive(sound)) return false;
    turnOn = R::ToString(R::NameOf(sound)) == L"turnon";
    return true;
}

}  // namespace ue_wrap::generator
