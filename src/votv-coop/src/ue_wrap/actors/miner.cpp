// ue_wrap/actors/miner.cpp -- see ue_wrap/actors/miner.h.

#include "ue_wrap/actors/miner.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/class_resolve.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <cwchar>

namespace ue_wrap::miner {
namespace {

namespace R = reflection;

constexpr wchar_t kRigClass[] = L"prop_miner_C";
constexpr uint8_t kClaimAction = 4;  // EInteractionActions: the rig's claim

// A hold name, the class the hand lane announces it as, and the class a refund spawns. A scrap metal is a plain prop_C
// whose row is `scrap_metal`, so its spawn names the row before the actor finishes; the GPU's and the cooler's classes
// carry their own row as the default.
struct ItemRow {
    const wchar_t* holdName;
    const wchar_t* cls;
};
constexpr ItemRow kItems[kItemCount] = {
    {L"scrap_metal", L"prop_C"},
    {L"miner_gpu", L"prop_gpu_C"},
    {L"miner_fan", L"prop_cooler_C"},
};

R::FName g_rigName{0, 0};
bool g_namesResolved = false;

// The rig's members: the five numbers' places per class, and the bit of `powered`, found on the first live rig.
R::InstanceOffset g_gpus{L"upg_gpu"}, g_slots{L"upg_slots"}, g_fans{L"upg_fans"}, g_points{L"accumulatedPoints"},
    g_heat{L"overheat"};
struct PoweredBit {
    int32_t offset = -1;
    uint8_t mask = 0;
    bool tried = false;
};
PoweredBit g_powered;

bool SameName(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

bool PoweredOf(void* rig) {
    if (g_powered.offset >= 0) return true;
    if (g_powered.tried) return false;
    g_powered.tried = true;
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(rig), L"powered", off, mask) || off < 0 || mask == 0) {
        UE_LOGE("miner: a rig's `powered` flag did not resolve -- no rig is read or written for the rest of the process");
        return false;
    }
    g_powered.offset = off;
    g_powered.mask = mask;
    return true;
}

template <class T>
T* Member(void* rig, R::InstanceOffset& slot) {
    const int32_t off = slot.Of(rig);
    return off >= 0 ? reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(rig) + off) : nullptr;
}

bool CallNoArgs(void* rig, const wchar_t* fn) {
    if (!IsMiner(rig)) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(rig), fn));
    return f.valid() && Call(rig, f);
}

void NameScrap(void*, void* actor) {
    prop::WritePropName(actor, fname_utils::StringToFName(kItems[kItemSlot].holdName));
}

struct RigWalk {
    RigFn fn;
    void* ctx;
};

}  // namespace

const wchar_t* ItemHoldName(uint8_t item) { return item < kItemCount ? kItems[item].holdName : nullptr; }
const wchar_t* ItemClass(uint8_t item) { return item < kItemCount ? kItems[item].cls : nullptr; }

int ItemOfHoldName(const std::wstring& name) {
    for (int i = 0; i < kItemCount; ++i)
        if (_wcsicmp(name.c_str(), kItems[i].holdName) == 0) return i;
    return -1;
}

bool ResolveNames() {
    if (g_namesResolved) return true;
    const R::FName rig = fname_utils::StringToFName(kRigClass);
    if (rig.ComparisonIndex == 0) return false;
    g_rigName = rig;
    g_namesResolved = true;
    return true;
}


bool IsMiner(void* obj) {
    if (!obj || !g_namesResolved) return false;
    void* cls = R::ClassOf(obj);
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (SameName(R::NameOf(cls), g_rigName)) return true;
        cls = R::SuperStructOf(cls);
    }
    return false;
}

bool Read(void* rig, Fields& out) {
    if (!IsMiner(rig) || !PoweredOf(rig)) return false;
    const int32_t* g = Member<int32_t>(rig, g_gpus);
    const int32_t* s = Member<int32_t>(rig, g_slots);
    const int32_t* f = Member<int32_t>(rig, g_fans);
    const int32_t* p = Member<int32_t>(rig, g_points);
    const float* h = Member<float>(rig, g_heat);
    if (!g || !s || !f || !p || !h) return false;
    out.gpus = *g;
    out.slots = *s;
    out.fans = *f;
    out.points = *p;
    out.overheat = *h;
    out.powered = (reinterpret_cast<const uint8_t*>(rig)[g_powered.offset] & g_powered.mask) != 0;
    return true;
}

bool Write(void* rig, const Fields& v) {
    if (!IsMiner(rig) || !PoweredOf(rig)) return false;
    int32_t* g = Member<int32_t>(rig, g_gpus);
    int32_t* s = Member<int32_t>(rig, g_slots);
    int32_t* f = Member<int32_t>(rig, g_fans);
    int32_t* p = Member<int32_t>(rig, g_points);
    float* h = Member<float>(rig, g_heat);
    if (!g || !s || !f || !p || !h) return false;
    *g = v.gpus;
    *s = v.slots;
    *f = v.fans;
    *p = v.points;
    *h = v.overheat;
    uint8_t& b = reinterpret_cast<uint8_t*>(rig)[g_powered.offset];
    b = v.powered ? static_cast<uint8_t>(b | g_powered.mask) : static_cast<uint8_t>(b & ~g_powered.mask);
    return true;
}

bool CallUpgrade(void* rig, uint8_t item) {
    if (!IsMiner(rig) || item >= kItemCount) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(rig), L"upgrade"));
    if (!f.valid()) return false;
    f.Set<R::FName>(L"upgrade", fname_utils::StringToFName(kItems[item].holdName));
    return Call(rig, f) && f.Get<bool>(L"success");
}

bool CallClaim(void* rig, void* player) {
    if (!IsMiner(rig) || !player) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(rig), L"actionOptionIndex"));
    if (!f.valid()) return false;
    f.Set<void*>(L"player", player);
    f.Set<uint8_t>(L"action", kClaimAction);
    return Call(rig, f);
}

bool CallUpd(void* rig) { return CallNoArgs(rig, L"upd"); }
bool CallPlugged(void* rig) { return CallNoArgs(rig, L"plugged"); }

bool ReadHold(void* player, std::wstring& name, void*& actor) {
    static R::InstanceOffset sName{L"holding_name"};
    static R::InstanceOffset sActor{L"holding_actor"};
    if (!player) return false;
    const int32_t nOff = sName.Of(player);
    const int32_t aOff = sActor.Of(player);
    if (nOff < 0 || aOff < 0) return false;
    name = R::ToString(*reinterpret_cast<const R::FName*>(reinterpret_cast<const char*>(player) + nOff));
    actor = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(player) + aOff);
    return true;
}

void ForEachRig(RigFn fn, void* ctx) {
    void* const cls = object_index::ClassByName(kRigClass);
    if (!cls || !fn) return;
    RigWalk w{fn, ctx};
    object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        const RigWalk* walk = static_cast<const RigWalk*>(c);
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) walk->fn(walk->ctx, obj);
    }, &w);
}

bool ReadRecord(const save_record::SaveRecord& rec, Fields& out) {
    if (rec.ints.empty() || rec.ints[0].size() < 4 || rec.floats.size() < 2 || rec.floats[1].empty()) return false;
    out.gpus = rec.ints[0][0];
    out.slots = rec.ints[0][1];
    out.fans = rec.ints[0][2];
    out.points = rec.ints[0][3];
    out.overheat = rec.floats[1][0];
    out.powered = false;
    return true;
}

void* SpawnItem(uint8_t item, const FVector& at) {
    if (item >= kItemCount) return nullptr;
    // The game loads a Blueprint class when something first needs it, and a host that never held this item has not: the
    // resolver loads it. The name comes from this file's own table, so that table is the bound; the caller spawns at once.
    const class_resolve::Resolved r =
        class_resolve::ResolveClass(kItems[item].cls, class_resolve::AnyFamily(), class_resolve::Load::Immediate);
    if (r.state != class_resolve::ClassState::Found || !r.cls) return nullptr;
    return engine::SpawnActor(r.cls, at, false, item == kItemSlot ? &NameScrap : nullptr, nullptr);
}

}  // namespace ue_wrap::miner
