// ue_wrap/devices/coord_tower.cpp -- see ue_wrap/devices/coord_tower.h. Offsets are the class's layout, the same in
// every world, and resolve once by name; verbs resolve on the tower in hand through the dispatch cache.

#include "ue_wrap/devices/coord_tower.h"

#include "ue_wrap/actors/prop.h"  // GetPropNameString
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"            // SpawnActor
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation
#include "ue_wrap/engine/engine_timeline.h"
#include "ue_wrap/engine/hit_result.h"

#include <algorithm>
#include <chrono>

namespace ue_wrap::coord_tower {
namespace {

namespace R = reflection;
using field_io::TArrayView;

constexpr const wchar_t* kClassName = L"coordRadarDish_C";

struct Bool { int32_t off = -1; uint8_t mask = 0; };

CachedObjRef g_cls;             // coordRadarDish_C; a class the map loads, so a new world may load a new one
int32_t  g_offId = -1;
Bool     g_broken, g_opened, g_anim, g_leverMoving;
int32_t  g_offFuses = -1;       // TArray<uint8>
int32_t  g_offLights = -1;      // TArray<bool> puzzleLights
// The look-at getActionOptions writes and the use reads.
Bool     g_lookButton, g_lookLever, g_lookFuse, g_lookRetract;
int32_t  g_offButtonIndex = -1, g_offFuseIndex = -1;
// The parts a use strikes: TArray<UPrimitiveComponent*> puzzle_buttons and fusesButtons, the lever's box, the
// panel's first retract button.
int32_t  g_offButtons = -1, g_offFuseSlots = -1, g_offLeverBox = -1, g_offRetract = -1;
int32_t  g_offTimeline = -1;    // UTimelineComponent* leverTL, read through ue_wrap/engine/engine_timeline
// Audio components by Sound. The game names its two fuse sounds the other way round: a pull plays
// audio_fuseInsert and an insert audio_fusePullout.
constexpr const wchar_t* kSoundFields[] = { L"audio_click", L"audio_success", L"audio_fail", L"audio_fuseInsert",
                                            L"audio_fusePullout" };
constexpr int kSoundCount = sizeof(kSoundFields) / sizeof(kSoundFields[0]);
int32_t  g_offSound[kSoundCount] = {};
bool     g_membersResolved = false;
bool     g_latchedOff = false;  // the class loaded without a member: it will not appear later
uint64_t g_missMs = 0;          // a class lookup that misses walks the object array; hold off after one

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint8_t* Base(void* obj) { return reinterpret_cast<uint8_t*>(obj); }

bool ReadBit(void* obj, const Bool& b) { return (Base(obj)[b.off] & b.mask) != 0; }

void WriteBit(void* obj, const Bool& b, bool v) {
    uint8_t& byte = Base(obj)[b.off];
    byte = v ? static_cast<uint8_t>(byte | b.mask) : static_cast<uint8_t>(byte & ~b.mask);
}

int32_t& IntAt(void* obj, int32_t off) { return *reinterpret_cast<int32_t*>(Base(obj) + off); }

void* ObjectAt(void* obj, int32_t off) { return *reinterpret_cast<void* const*>(Base(obj) + off); }

TArrayView& ArrayAt(void* obj, int32_t off) { return *reinterpret_cast<TArrayView*>(Base(obj) + off); }

void* ArrayObject(void* obj, int32_t off, int32_t index) {
    const TArrayView& a = ArrayAt(obj, off);
    if (!a.data || index < 0 || index >= a.num) return nullptr;
    void* o = reinterpret_cast<void* const*>(a.data)[index];
    return (o && R::IsLive(o)) ? o : nullptr;
}

// Resolve every member from `cls`, the loaded coordRadarDish_C; a member missing from it latches the wrapper off.
bool ResolveMembers(void* cls) {
    const wchar_t* missing = nullptr;
    auto need = [&](int32_t off, const wchar_t* name) { if (off < 0 && !missing) missing = name; return off; };
    auto needBool = [&](Bool& b, const wchar_t* name) {
        if (!R::FindBoolProperty(cls, name, b.off, b.mask) || !b.mask) { b.off = -1; if (!missing) missing = name; }
    };
    g_offId = need(R::FindPropertyOffset(cls, L"id"), L"id");
    needBool(g_broken, L"isBroken");
    needBool(g_opened, L"opened");
    needBool(g_anim, L"isAnim");
    needBool(g_leverMoving, L"leverMoving");
    g_offFuses = need(R::FindPropertyOffset(cls, L"fuses"), L"fuses");
    g_offLights = need(R::FindPropertyOffset(cls, L"puzzleLights"), L"puzzleLights");
    needBool(g_lookButton, L"isLookAtButton");
    needBool(g_lookLever, L"isLookAtLever");
    needBool(g_lookFuse, L"isLookAtFuse");
    needBool(g_lookRetract, L"isLookAtRetract");
    g_offButtonIndex = need(R::FindPropertyOffset(cls, L"lookAtButtonIndex"), L"lookAtButtonIndex");
    g_offFuseIndex = need(R::FindPropertyOffset(cls, L"lookAtFuseIndex"), L"lookAtFuseIndex");
    g_offButtons = need(R::FindPropertyOffset(cls, L"puzzle_buttons"), L"puzzle_buttons");
    g_offFuseSlots = need(R::FindPropertyOffset(cls, L"fusesButtons"), L"fusesButtons");
    g_offLeverBox = need(R::FindPropertyOffset(cls, L"buttonLever"), L"buttonLever");
    g_offRetract = need(R::FindPropertyOffset(cls, L"button_retract1"), L"button_retract1");
    g_offTimeline = need(R::FindPropertyOffset(cls, L"leverTL"), L"leverTL");
    for (int i = 0; i < kSoundCount; ++i) g_offSound[i] = need(R::FindPropertyOffset(cls, kSoundFields[i]),
                                                               kSoundFields[i]);
    if (missing) {
        g_latchedOff = true;
        UE_LOGW("coord_tower: coordRadarDish_C is loaded but %ls did not resolve -- the tower reads and writes stay "
                "off; game version mismatch?", missing);
        return false;
    }
    g_membersResolved = true;
    UE_LOGI("coord_tower: resolved (id=0x%X isBroken=0x%X opened=0x%X fuses=0x%X puzzleLights=0x%X leverTL=0x%X)",
            g_offId, g_broken.off, g_opened.off, g_offFuses, g_offLights, g_offTimeline);
    return true;
}

// Take `cls` as the class. Members resolve from the first one taken: a class the same map loads again has the
// same layout. Reads the class's property chain only, never the object array.
bool Adopt(void* cls) {
    if (!g_membersResolved && !ResolveMembers(cls)) return false;
    g_cls.Set(cls);
    return true;
}

struct Collect { void** out; int32_t cap; int32_t n; };

void CollectOne(void* ctx, void* obj, int32_t index) {
    auto* c = static_cast<Collect*>(ctx);
    if (c->n >= c->cap || !obj) return;
    // An index member may still be loading, under construction or dying; the slot's flags say so.
    if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
    if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    c->out[c->n++] = obj;
}

bool Usable(void* tower) { return tower && g_membersResolved && R::IsLive(tower); }

bool CallNamed(void* tower, const wchar_t* fn) {
    return Usable(tower) && component_calls::CallParamlessNamed(tower, fn);
}

// The component a look-at names, the part a use strikes.
void* PartComponent(void* tower, const LookAt& at) {
    switch (at.part) {
    case Part::Button:  return ArrayObject(tower, g_offButtons, at.index);
    case Part::Fuse:    return ArrayObject(tower, g_offFuseSlots, at.index);
    case Part::Lever:   { void* c = ObjectAt(tower, g_offLeverBox); return (c && R::IsLive(c)) ? c : nullptr; }
    case Part::Retract: { void* c = ObjectAt(tower, g_offRetract); return (c && R::IsLive(c)) ? c : nullptr; }
    default:            return nullptr;
    }
}

struct SavedLook {
    bool button, lever, fuse, retract;
    int32_t buttonIndex, fuseIndex;
};

SavedLook SaveLook(void* tower) {
    return {ReadBit(tower, g_lookButton), ReadBit(tower, g_lookLever), ReadBit(tower, g_lookFuse),
            ReadBit(tower, g_lookRetract), IntAt(tower, g_offButtonIndex), IntAt(tower, g_offFuseIndex)};
}

void RestoreLook(void* tower, const SavedLook& s) {
    WriteBit(tower, g_lookButton, s.button);
    WriteBit(tower, g_lookLever, s.lever);
    WriteBit(tower, g_lookFuse, s.fuse);
    WriteBit(tower, g_lookRetract, s.retract);
    IntAt(tower, g_offButtonIndex) = s.buttonIndex;
    IntAt(tower, g_offFuseIndex) = s.fuseIndex;
}

// What getActionOptions writes for `at`: one flag on, the others off, and the index of a button or a fuse.
void WriteLook(void* tower, const LookAt& at) {
    WriteBit(tower, g_lookButton, at.part == Part::Button);
    WriteBit(tower, g_lookLever, at.part == Part::Lever);
    WriteBit(tower, g_lookFuse, at.part == Part::Fuse);
    WriteBit(tower, g_lookRetract, at.part == Part::Retract);
    if (at.part == Part::Button) IntAt(tower, g_offButtonIndex) = at.index;
    if (at.part == Part::Fuse) IntAt(tower, g_offFuseIndex) = at.index;
}

}  // namespace

bool EnsureResolved() {
    if (g_membersResolved && g_cls.Alive()) return true;
    if (g_latchedOff) return false;
    const uint64_t now = NowMs();
    if (g_missMs != 0 && now - g_missMs < 2000) return false;
    void* cls = object_index::ClassByName(kClassName);
    if (!cls) { g_missMs = now; return false; }
    g_missMs = 0;
    return Adopt(cls);
}

int32_t ReadAll(void** out, int32_t cap) {
    if (!out || cap <= 0 || !EnsureResolved()) return -1;
    Collect c{out, cap, 0};
    object_index::ForEachInstance(g_cls.Raw(), &CollectOne, &c);
    std::sort(out, out + c.n, [](void* a, void* b) { return IntAt(a, g_offId) < IntAt(b, g_offId); });
    return c.n;
}

int32_t IdOf(void* tower) {
    if (!tower || g_latchedOff) return -1;
    void* cls = R::ClassOf(tower);
    if (!cls) return -1;
    if (!g_membersResolved || !g_cls.Alive() || cls != g_cls.Raw()) {
        // The tower in hand names its class, so the lookup that would walk the object array is never needed
        // here: a name compare, and the property chain on the first one.
        if (!R::NameEquals(R::NameOf(cls), kClassName) || !Adopt(cls)) return -1;
    }
    return IntAt(tower, g_offId);
}

bool Read(void* tower, State& out) {
    if (!Usable(tower)) return false;
    out = State{};
    out.id = IntAt(tower, g_offId);
    out.isBroken = ReadBit(tower, g_broken);
    out.opened = ReadBit(tower, g_opened);
    out.isAnim = ReadBit(tower, g_anim);
    out.leverMoving = ReadBit(tower, g_leverMoving);
    engine_timeline::State tl;
    if (engine_timeline::Read(ObjectAt(tower, g_offTimeline), tl)) {
        // Forward and past its start, or forward and running: a fresh timeline stands forward at 0, down.
        out.leverUp = !tl.reverse && (tl.playing || tl.position > 0.0f);
    }
    const TArrayView& f = ArrayAt(tower, g_offFuses);
    out.fuseCount = static_cast<uint8_t>(f.data ? std::clamp(f.num, 0, kMaxFuses) : 0);
    for (int i = 0; i < out.fuseCount; ++i) out.fuses[i] = f.data[i];
    const TArrayView& p = ArrayAt(tower, g_offLights);
    out.lightCount = static_cast<uint8_t>(p.data ? std::clamp(p.num, 0, kMaxLights) : 0);
    for (int i = 0; i < out.lightCount; ++i) out.lights[i] = p.data[i] != 0;
    return true;
}

bool WriteBroken(void* tower, bool broken) {
    if (!Usable(tower)) return false;
    WriteBit(tower, g_broken, broken);
    return true;
}

bool WriteLeverMoving(void* tower, bool moving) {
    if (!Usable(tower)) return false;
    WriteBit(tower, g_leverMoving, moving);
    return true;
}

bool WriteFuse(void* tower, int32_t slot, uint8_t value) {
    if (!Usable(tower)) return false;
    TArrayView& f = ArrayAt(tower, g_offFuses);
    if (!f.data || slot < 0 || slot >= f.num) return false;
    f.data[slot] = value;
    return true;
}

bool WriteLights(void* tower, const bool* lights, int32_t count) {
    if (!Usable(tower) || !lights) return false;
    TArrayView& p = ArrayAt(tower, g_offLights);
    if (!p.data || p.num != count) return false;
    for (int32_t i = 0; i < count; ++i) p.data[i] = lights[i] ? 1 : 0;
    return true;
}

bool UpdBroken(void* tower) { return CallNamed(tower, L"updBroken"); }
bool UpdPuzzle(void* tower) { return CallNamed(tower, L"updPuzzle"); }
bool UpdFuses(void* tower) { return CallNamed(tower, L"updFuses"); }
bool SolvePuzzle(void* tower) { return CallNamed(tower, L"solvePuzzle"); }
bool Scramble(void* tower) { return CallNamed(tower, L"Scramble Radar Dish"); }

bool MoveLever(void* tower, bool up) {
    if (!Usable(tower)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(tower), L"moveLever");
    ParamFrame f(fn);
    return fn && f.valid() && f.Set<bool>(L"Condition", up) && Call(tower, f);
}

bool Play(void* tower, Sound sound) {
    const int i = static_cast<int>(sound);
    if (!Usable(tower) || i < 0 || i >= kSoundCount) return false;
    void* comp = ObjectAt(tower, g_offSound[i]);
    return engine::PlayAudioComponent(comp);
}

bool ReadLookAt(void* tower, LookAt& out) {
    if (!Usable(tower)) return false;
    out = LookAt{};
    // getActionOptions tests the button first, then the lever, the fuse and the retract; the use tests the
    // retract first, then the fuse, the button and the lever. Only one is ever set by a trace.
    if (ReadBit(tower, g_lookRetract)) out.part = Part::Retract;
    else if (ReadBit(tower, g_lookFuse)) out = {Part::Fuse, IntAt(tower, g_offFuseIndex)};
    else if (ReadBit(tower, g_lookButton)) out = {Part::Button, IntAt(tower, g_offButtonIndex)};
    else if (ReadBit(tower, g_lookLever)) out.part = Part::Lever;
    return true;
}

bool ReadFuseLook(void* tower, int32_t& slot) {
    if (!Usable(tower)) return false;
    slot = IntAt(tower, g_offFuseIndex);
    return true;
}

bool Use(void* tower, void* player, const LookAt& at) {
    if (!Usable(tower) || !player) return false;
    void* part = PartComponent(tower, at);
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(tower), L"actionOptionIndex");
    if (!part || !fn) return false;
    const SavedLook saved = SaveLook(tower);
    WriteLook(tower, at);
    const FVector where = engine::GetComponentLocation(part);
    ParamFrame f(fn);
    const bool ok = f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", tower, part, where) &&
                    f.Set<uint8_t>(L"action", 4) && f.Set<void*>(L"lookAtComponent", part) && Call(tower, f);
    if (R::IsLive(tower)) RestoreLook(tower, saved);
    return ok;
}

bool PartLocation(void* tower, const LookAt& at, FVector& out) {
    if (!Usable(tower)) return false;
    void* part = PartComponent(tower, at);
    if (!part) return false;
    out = engine::GetComponentLocation(part);
    return true;
}

bool IsFuse(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* cls = object_index::ClassByName(L"prop_fuse_C");
    return cls && R::ClassOf(actor) == cls;
}

void* SpawnFuse(const FVector& at) {
    void* cls = object_index::ClassByName(L"prop_fuse_C");
    return cls ? engine::SpawnActor(cls, at) : nullptr;
}

bool IsPulledFuse(void* actor) {
    return actor && R::IsLive(actor) && prop::GetPropNameString(actor) == L"fuse_0";
}

bool InsertFuse(void* tower, void* player, int32_t slot, void* fuse) {
    if (!Usable(tower) || !player || !fuse) return false;
    const LookAt at{Part::Fuse, slot};
    void* part = PartComponent(tower, at);
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(tower), L"playerUsedOn");
    if (!part || !fn) return false;
    const SavedLook saved = SaveLook(tower);
    WriteLook(tower, at);
    const FVector where = engine::GetComponentLocation(part);
    ParamFrame f(fn);
    const bool ok = f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", tower, part, where) &&
                    f.Set<void*>(L"lookAtComponent", part) && f.Set<void*>(L"holdObject", fuse) && Call(tower, f);
    if (R::IsLive(tower)) RestoreLook(tower, saved);
    return ok;
}

}  // namespace ue_wrap::coord_tower
