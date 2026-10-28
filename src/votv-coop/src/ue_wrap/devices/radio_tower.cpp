// ue_wrap/devices/radio_tower.cpp -- see ue_wrap/devices/radio_tower.h. Offsets are the class's layout, the same
// in every world, and resolve once by name; verbs resolve on the tower in hand through the dispatch cache.
// The BP facts (bp_cpp radiotower_C, line numbers are its pseudo-C++): getActionOptions recomputes every
// look-at flag on every look (:1803-1827); actionOptionIndex acts on the fuse look, the button, the reset lever,
// then the confirm lever, and is ignored while `animation` (:510-568); playerUsedOn is gated the same (:655);
// setBroken is (isBroken, pullLever) (:1163); scramble, tryToFix and completeMinigame take no argument (:1496,
// :1599, :2185).

#include "ue_wrap/devices/radio_tower.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation, PlayAudioComponent
#include "ue_wrap/engine/engine_timeline.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>

namespace ue_wrap::radio_tower {
namespace {

namespace R = reflection;
using field_io::TArrayView;

constexpr const wchar_t* kClassName = L"radiotower_C";

struct Bool { int32_t off = -1; uint8_t mask = 0; };

CachedObjRef g_cls;             // radiotower_C; a class the map loads, so a new world may load a new one
CachedObjRef g_tower;           // the gamemode's radiotower
Bool     g_broken, g_anim, g_skipPuzzle;
int32_t  g_offFuses = -1;       // TArray<uint8>
int32_t  g_offCells = -1;       // TArray<bool> puzzleData
int32_t  g_offSize = -1;        // FIntPoint puzzleSize
// The look-at getActionOptions writes and the use reads.
Bool     g_lookButton, g_lookConfirm, g_lookReset, g_lookFuse;
int32_t  g_offButtonIndex = -1, g_offFuseIndex = -1;
// The parts a use strikes: TArray<UPrimitiveComponent*> puzzleButtons and fusesButtons, the two levers' boxes.
int32_t  g_offButtons = -1, g_offFuseSlots = -1, g_offConfirmBox = -1, g_offResetBox = -1;
int32_t  g_offLeverTl = -1, g_offResetTl = -1;  // UTimelineComponent* TL_lever, TL_reset
// Audio components by Sound. The game names its two fuse sounds the other way round: a pull plays
// audio_fuseInsert and an insert audio_fusePullout.
constexpr const wchar_t* kSoundFields[] = { L"audio_click", L"audio_success", L"audio_fail", L"audio_fuseInsert",
                                            L"audio_fusePullout", L"audio_lever", L"audio_scramble" };
constexpr int kSoundCount = sizeof(kSoundFields) / sizeof(kSoundFields[0]);
int32_t  g_offSound[kSoundCount] = {};
int32_t  g_offGmTower = -1;     // mainGamemode_C::radiotower
void*    g_gmCls = nullptr;     // the gamemode class that offset was read from
bool     g_membersResolved = false;
bool     g_latchedOff = false;  // the class loaded without a member: it will not appear later

uint8_t* Base(void* obj) { return reinterpret_cast<uint8_t*>(obj); }

bool ReadBit(void* obj, const Bool& b) { return (Base(obj)[b.off] & b.mask) != 0; }

void WriteBit(void* obj, const Bool& b, bool v) {
    uint8_t& byte = Base(obj)[b.off];
    byte = v ? static_cast<uint8_t>(byte | b.mask) : static_cast<uint8_t>(byte & ~b.mask);
}

int32_t& IntAt(void* obj, int32_t off) { return *reinterpret_cast<int32_t*>(Base(obj) + off); }

void* ObjectAt(void* obj, int32_t off) { return *reinterpret_cast<void* const*>(Base(obj) + off); }

TArrayView& ArrayAt(void* obj, int32_t off) { return *reinterpret_cast<TArrayView*>(Base(obj) + off); }

void* LiveObjectAt(void* obj, int32_t off) {
    void* o = ObjectAt(obj, off);
    return (o && R::IsLive(o)) ? o : nullptr;
}

void* ArrayObject(void* obj, int32_t off, int32_t index) {
    const TArrayView& a = ArrayAt(obj, off);
    if (!a.data || index < 0 || index >= a.num) return nullptr;
    void* o = reinterpret_cast<void* const*>(a.data)[index];
    return (o && R::IsLive(o)) ? o : nullptr;
}

// Resolve every member from `cls`, the loaded radiotower_C; a member missing from it latches the wrapper off.
bool ResolveMembers(void* cls) {
    const wchar_t* missing = nullptr;
    auto need = [&](int32_t off, const wchar_t* name) { if (off < 0 && !missing) missing = name; return off; };
    auto needBool = [&](Bool& b, const wchar_t* name) {
        if (!R::FindBoolProperty(cls, name, b.off, b.mask) || !b.mask) { b.off = -1; if (!missing) missing = name; }
    };
    needBool(g_broken, L"isBroken");
    needBool(g_anim, L"animation");
    needBool(g_skipPuzzle, L"skipPuzzle");
    g_offFuses = need(R::FindPropertyOffset(cls, L"fuses"), L"fuses");
    g_offCells = need(R::FindPropertyOffset(cls, L"puzzleData"), L"puzzleData");
    g_offSize = need(R::FindPropertyOffset(cls, L"puzzleSize"), L"puzzleSize");
    needBool(g_lookButton, L"isLookAtButton");
    needBool(g_lookConfirm, L"lookAtConfirm");
    needBool(g_lookReset, L"lookAtReset");
    needBool(g_lookFuse, L"lookAtFuse");
    g_offButtonIndex = need(R::FindPropertyOffset(cls, L"lookAtIndex"), L"lookAtIndex");
    g_offFuseIndex = need(R::FindPropertyOffset(cls, L"lookAtFuseIndex"), L"lookAtFuseIndex");
    g_offButtons = need(R::FindPropertyOffset(cls, L"puzzleButtons"), L"puzzleButtons");
    g_offFuseSlots = need(R::FindPropertyOffset(cls, L"fusesButtons"), L"fusesButtons");
    g_offConfirmBox = need(R::FindPropertyOffset(cls, L"button_confirm"), L"button_confirm");
    g_offResetBox = need(R::FindPropertyOffset(cls, L"button_reset"), L"button_reset");
    g_offLeverTl = need(R::FindPropertyOffset(cls, L"TL_lever"), L"TL_lever");
    g_offResetTl = need(R::FindPropertyOffset(cls, L"TL_reset"), L"TL_reset");
    for (int i = 0; i < kSoundCount; ++i) g_offSound[i] = need(R::FindPropertyOffset(cls, kSoundFields[i]),
                                                               kSoundFields[i]);
    if (missing) {
        g_latchedOff = true;
        UE_LOGW("radio_tower: radiotower_C is loaded but %ls did not resolve -- the tower reads and writes stay "
                "off; game version mismatch?", missing);
        return false;
    }
    g_membersResolved = true;
    UE_LOGI("radio_tower: resolved (isBroken=0x%X animation=0x%X fuses=0x%X puzzleData=0x%X TL_lever=0x%X)",
            g_broken.off, g_anim.off, g_offFuses, g_offCells, g_offLeverTl);
    return true;
}

// Take `cls` as the class. Members resolve from the first one taken: a class the same map loads again has the
// same layout. Reads the class's property chain only, never the object array.
bool Adopt(void* cls) {
    if (!g_membersResolved && !ResolveMembers(cls)) return false;
    g_cls.Set(cls);
    return true;
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
    case Part::Confirm: return LiveObjectAt(tower, g_offConfirmBox);
    case Part::Reset:   return LiveObjectAt(tower, g_offResetBox);
    default:            return nullptr;
    }
}

struct SavedLook {
    bool button, confirm, reset, fuse;
    int32_t buttonIndex, fuseIndex;
};

SavedLook SaveLook(void* tower) {
    return {ReadBit(tower, g_lookButton), ReadBit(tower, g_lookConfirm), ReadBit(tower, g_lookReset),
            ReadBit(tower, g_lookFuse), IntAt(tower, g_offButtonIndex), IntAt(tower, g_offFuseIndex)};
}

void RestoreLook(void* tower, const SavedLook& s) {
    WriteBit(tower, g_lookButton, s.button);
    WriteBit(tower, g_lookConfirm, s.confirm);
    WriteBit(tower, g_lookReset, s.reset);
    WriteBit(tower, g_lookFuse, s.fuse);
    IntAt(tower, g_offButtonIndex) = s.buttonIndex;
    IntAt(tower, g_offFuseIndex) = s.fuseIndex;
}

// What getActionOptions writes for `at`: one flag on, the others off, and the index of a button or a fuse.
void WriteLook(void* tower, const LookAt& at) {
    WriteBit(tower, g_lookButton, at.part == Part::Button);
    WriteBit(tower, g_lookConfirm, at.part == Part::Confirm);
    WriteBit(tower, g_lookReset, at.part == Part::Reset);
    WriteBit(tower, g_lookFuse, at.part == Part::Fuse);
    if (at.part == Part::Button) IntAt(tower, g_offButtonIndex) = at.index;
    if (at.part == Part::Fuse) IntAt(tower, g_offFuseIndex) = at.index;
}

// The gamemode's radiotower field, its offset read once per gamemode class.
void* GamemodeTower() {
    void* gm = world_singleton::Gamemode();
    void* gmCls = gm ? R::ClassOf(gm) : nullptr;
    if (!gmCls) return nullptr;
    if (gmCls != g_gmCls) {
        g_offGmTower = R::FindPropertyOffset(gmCls, L"radiotower");
        g_gmCls = gmCls;
    }
    if (g_offGmTower < 0) return nullptr;
    void* rt = ObjectAt(gm, g_offGmTower);
    return (rt && R::IsLive(rt)) ? rt : nullptr;
}

}  // namespace

void* Instance() {
    if (g_latchedOff) return nullptr;
    if (g_membersResolved && g_tower.Alive()) return g_tower.Raw();
    void* rt = GamemodeTower();
    if (!rt || !Adopt(R::ClassOf(rt))) return nullptr;
    g_tower.Set(rt);
    return rt;
}

bool IsTower(void* obj) {
    if (!obj || g_latchedOff) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    if (!g_membersResolved || !g_cls.Alive() || cls != g_cls.Raw()) {
        // The object in hand names its class: a name compare, and the property chain on the first one.
        if (!R::NameEquals(R::NameOf(cls), kClassName) || !Adopt(cls)) return false;
    }
    return true;
}

bool Read(void* tower, State& out) {
    if (!Usable(tower)) return false;
    out = State{};
    out.isBroken = ReadBit(tower, g_broken);
    out.animation = ReadBit(tower, g_anim);
    out.skipPuzzle = ReadBit(tower, g_skipPuzzle);
    engine_timeline::State tl;
    if (engine_timeline::Read(LiveObjectAt(tower, g_offLeverTl), tl)) {
        out.leverPlaying = tl.playing;
        out.leverForward = !tl.reverse;
    }
    if (engine_timeline::Read(LiveObjectAt(tower, g_offResetTl), tl)) out.resetPlaying = tl.playing;
    const TArrayView& f = ArrayAt(tower, g_offFuses);
    out.fuseCount = static_cast<uint8_t>(f.data ? std::clamp(f.num, 0, kMaxFuses) : 0);
    for (int i = 0; i < out.fuseCount; ++i) out.fuses[i] = f.data[i];
    out.width = static_cast<uint8_t>(std::clamp(IntAt(tower, g_offSize), 0, kMaxCells));
    out.height = static_cast<uint8_t>(std::clamp(IntAt(tower, g_offSize + 4), 0, kMaxCells));
    const TArrayView& p = ArrayAt(tower, g_offCells);
    out.cellCount = static_cast<uint8_t>(p.data ? std::clamp(p.num, 0, kMaxCells) : 0);
    for (int i = 0; i < out.cellCount; ++i) out.cells[i] = p.data[i] != 0;
    return true;
}

bool WriteAnimation(void* tower, bool animation) {
    if (!Usable(tower)) return false;
    WriteBit(tower, g_anim, animation);
    return true;
}

bool WriteFuse(void* tower, int32_t slot, uint8_t value) {
    if (!Usable(tower)) return false;
    TArrayView& f = ArrayAt(tower, g_offFuses);
    if (!f.data || slot < 0 || slot >= f.num) return false;
    f.data[slot] = value;
    return true;
}

bool WriteCells(void* tower, const bool* cells, int32_t count) {
    if (!Usable(tower) || !cells) return false;
    TArrayView& p = ArrayAt(tower, g_offCells);
    if (!p.data || p.num != count) return false;
    for (int32_t i = 0; i < count; ++i) p.data[i] = cells[i] ? 1 : 0;
    return true;
}

bool UpdPuzzle(void* tower) { return CallNamed(tower, L"updPuzzle"); }
bool UpdFuses(void* tower) { return CallNamed(tower, L"updFuses"); }
bool CompleteMinigame(void* tower) { return CallNamed(tower, L"completeMinigame"); }
bool Scramble(void* tower) { return CallNamed(tower, L"scramble"); }

bool SetBroken(void* tower, bool broken, bool pullLever) {
    if (!Usable(tower)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(tower), L"setBroken");
    ParamFrame f(fn);
    return fn && f.valid() && f.Set<bool>(L"isBroken", broken) && f.Set<bool>(L"pullLever", pullLever) &&
           Call(tower, f);
}

bool PlayLever(void* tower, Lever lever, bool forward) {
    if (!Usable(tower)) return false;
    if (lever == Lever::Reset) {
        return forward && engine_timeline::PlayFromStart(LiveObjectAt(tower, g_offResetTl));
    }
    void* tl = LiveObjectAt(tower, g_offLeverTl);
    return forward ? engine_timeline::PlayFromStart(tl) : engine_timeline::ReverseFromEnd(tl);
}

bool Play(void* tower, Sound sound) {
    const int i = static_cast<int>(sound);
    if (!Usable(tower) || i < 0 || i >= kSoundCount) return false;
    return engine::PlayAudioComponent(ObjectAt(tower, g_offSound[i]));
}

bool Alert(void* tower, bool damagedHealth) {
    if (!Usable(tower)) return false;
    void* console = world_singleton::Find(L"ui_console_C");
    void* fn = console ? R::FindDispatchFunctionCached(R::ClassOf(console), L"radioTowerBroken") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"radiotower", tower) && f.Set<bool>(L"damagedHealth", damagedHealth) &&
           Call(console, f);
}

bool ReadLookAt(void* tower, LookAt& out) {
    if (!Usable(tower)) return false;
    out = LookAt{};
    // getActionOptions tests the button, the confirm lever, the reset lever, then the fuse; the use tests the fuse
    // first, then the button, the reset lever and the confirm lever. Only one is ever set by a trace.
    if (ReadBit(tower, g_lookFuse)) out = {Part::Fuse, IntAt(tower, g_offFuseIndex)};
    else if (ReadBit(tower, g_lookButton)) out = {Part::Button, IntAt(tower, g_offButtonIndex)};
    else if (ReadBit(tower, g_lookReset)) out.part = Part::Reset;
    else if (ReadBit(tower, g_lookConfirm)) out.part = Part::Confirm;
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
    // radiotower playerUsedOn (:741) reads the player, the hit's component and holdObject; holdPropName stays empty.
    const bool ok = f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", tower, part, where) &&
                    f.Set<void*>(L"lookAtComponent", part) && f.Set<void*>(L"holdObject", fuse) && Call(tower, f);
    if (R::IsLive(tower)) RestoreLook(tower, saved);
    return ok;
}

}  // namespace ue_wrap::radio_tower
