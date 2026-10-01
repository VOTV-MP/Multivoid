// ue_wrap/devices/power_control.cpp -- see ue_wrap/devices/power_control.h. Offsets are the class's layout, the
// same in every world, and resolve once by name; verbs and classes are looked up where they are used.

#include "ue_wrap/devices/power_control.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"            // TryGetActorLocation
#include "ue_wrap/engine/engine_audio.h"      // PlaySoundAtLocation
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/world/world_singleton.h"
#include "coop/text/i18n.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ue_wrap::power_control {
namespace {

namespace R = reflection;
namespace P = profile;

// The five breakers, `bit` their place in the mask, in field order.
struct Sys {
    int            bit;
    const wchar_t* pressName;
    const wchar_t* leverName;  // the lever component the press compares its trace against
    int32_t        pressOff;   // resolved lazily (game-thread serial)
    int32_t        leverOff;
};

Sys g_sys[] = {
    { 0, L"press_coord", L"power_coordinates", -1, -1 },
    { 1, L"press_downl", L"power_downloading", -1, -1 },
    { 2, L"press_play",  L"power_playing",     -1, -1 },
    { 3, L"press_calc",  L"power_calculating", -1, -1 },
    { 4, L"press_light", L"power_light",       -1, -1 },
};
std::atomic<bool> g_resolved{false};
bool     g_layoutRefused = false;  // the class loaded but a field did not: permanent for this build
uint64_t g_nextTryMs = 0;
int32_t  g_disabledOff = -1;
int32_t  g_waterloggedOff = -1;
int32_t  g_offPanel = -1;          // mainGamemode_C.powerControl

// The game mode's unit flags, in UnitPower's order, and its powerUsage: the drill's reading.
struct GmBool { const wchar_t* name; int32_t off = -1; uint8_t mask = 0; };
GmBool  g_usesp[5] = {{L"usesp_calc"}, {L"usesp_downl"}, {L"usesp_coords"}, {L"usesp_play"}, {L"usesp_light"}};
int32_t g_offUsage = -1;

// A TArray<AActor*> member of the panel: its data, then its count and capacity.
struct ActorArray {
    void** data;
    int32_t num;
    int32_t max;
};

// One such member, read by name from the live class once; a class without it lacks it for the process.
struct ArrayMember {
    const wchar_t* name;
    int32_t off = -1;
    bool missing = false;

    bool Read(void* p, std::vector<void*>& out) {
        out.clear();
        if (!p || missing) return false;
        if (off < 0) {
            off = R::FindPropertyOffset(R::ClassOf(p), name);
            if (off < 0) {
                missing = true;
                UE_LOGW("power_control: powerControl_C has no %ls -- what a blackout reaches is out of reach", name);
                return false;
            }
        }
        const auto* a = reinterpret_cast<const ActorArray*>(reinterpret_cast<const uint8_t*>(p) + off);
        if (a->num < 0 || a->num > 4096 || (a->num > 0 && !a->data)) return false;
        for (int32_t i = 0; i < a->num; ++i)
            if (a->data[i] && R::IsLive(a->data[i])) out.push_back(a->data[i]);
        return true;
    }
};
ArrayMember g_lightRoots{L"lighRoots"};
ArrayMember g_blackoutDoors{L"doorsOpen"};
ArrayMember g_servers{coop::i18n::TrW(L"servers")};

// The stationTurnon sound, an asset held while its slot and serial still hold it.
CachedObjRef g_turnOnCue;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool& BoolAt(void* p, int32_t off) { return *reinterpret_cast<bool*>(reinterpret_cast<char*>(p) + off); }

void* ObjectField(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    void* v = off < 0 ? nullptr : *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(obj) + off);
    return (v && R::IsLive(v)) ? v : nullptr;
}

// The hum: an AmbientSound actor, whose root, and the component the panel switches, is its AudioComponent.
void* HumComponent(void* p) { return ObjectField(ObjectField(p, L"serversSound"), L"AudioComponent"); }

bool ComponentActive(void* comp, bool& active) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!comp || !R::FindBoolProperty(R::ClassOf(comp), L"bIsActive", off, mask)) return false;
    active = (*(reinterpret_cast<const uint8_t*>(comp) + off) & mask) != 0;
    return true;
}

bool CallVerb(void* p, const wchar_t* verb) {
    return p && g_resolved.load(std::memory_order_acquire) && component_calls::CallParamlessNamed(p, verb);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    if (g_layoutRefused) return false;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;

    void* cls = object_index::ClassByName(L"powerControl_C");
    void* gmCls = object_index::ClassByName(P::name::GamemodeClass);
    if (!cls || !gmCls) return false;  // not streamed in yet; retried a second later

    // Every offset and the apply verb resolve BY NAME, and a miss refuses the wrapper rather than falling back
    // on an address this build happens to use: the lane writes bools at these offsets, so a recook that moved a
    // field would turn a fallback into a blind write. The class IS loaded here, so a miss is a fact about this
    // game build, latched and said once.
    auto refuse = [&](const wchar_t* what) {
        g_layoutRefused = true;
        UE_LOGE("power: %ls did not resolve on a loaded powerControl_C -- the power panel lane is OFF for this "
                "game build (no blind writes at a stale offset)", what);
        return false;
    };

    for (auto& s : g_sys) {
        s.pressOff = R::FindPropertyOffset(cls, s.pressName);
        if (s.pressOff < 0) return refuse(s.pressName);
        s.leverOff = R::FindPropertyOffset(cls, s.leverName);  // the host runs a client's lever press through it
        if (s.leverOff < 0) return refuse(s.leverName);
    }
    const int32_t disabledOff = R::FindPropertyOffset(cls, L"disabled");
    if (disabledOff < 0) return refuse(L"disabled");
    const int32_t waterloggedOff = R::FindPropertyOffset(cls, L"waterlogged");
    if (waterloggedOff < 0) return refuse(L"waterlogged");
    if (!R::FindDispatchFunctionCached(cls, L"buttonsVisibility")) return refuse(L"buttonsVisibility()");
    if (!R::FindDispatchFunctionCached(cls, L"playSND"))
        UE_LOGW("power: playSND not found -- a lever press mirrored from another peer is silent");
    const int32_t offPanel = R::FindPropertyOffset(gmCls, L"powerControl");
    if (offPanel < 0) return refuse(L"mainGamemode_C::powerControl");
    for (auto& b : g_usesp) R::FindBoolProperty(gmCls, b.name, b.off, b.mask);  // the drill's reading only
    g_offUsage = R::FindPropertyOffset(gmCls, L"powerUsage");

    g_offPanel = offPanel;
    g_disabledOff = disabledOff;
    g_waterloggedOff = waterloggedOff;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("power: resolved powerControl_C disabled@0x%04X waterlogged@0x%04X gamemode.powerControl@0x%04X",
            disabledOff, waterloggedOff, offPanel);
    return true;
}

bool IsPowerControl(void* obj) {
    if (!obj || !g_resolved.load(std::memory_order_acquire)) return false;
    void* cls = R::ClassOf(obj);
    void* bases[1] = { object_index::ClassByName(L"powerControl_C") };
    return cls && bases[0] && R::IsDescendantOfAny(cls, bases, 1);
}

void* Panel() {
    if (!g_resolved.load(std::memory_order_acquire)) return nullptr;
    void* gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    void* p = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(gm) + g_offPanel);
    return (p && R::IsLive(p) && IsPowerControl(p)) ? p : nullptr;
}

bool ReadPress(void* p, uint8_t& mask) {
    if (!p || !g_resolved.load(std::memory_order_acquire)) return false;
    uint8_t m = 0;
    for (auto& s : g_sys)
        if (BoolAt(p, s.pressOff)) m |= static_cast<uint8_t>(1u << s.bit);
    mask = m;
    return true;
}

bool WritePress(void* p, uint8_t mask) {
    if (!p || !g_resolved.load(std::memory_order_acquire)) return false;
    for (auto& s : g_sys) BoolAt(p, s.pressOff) = (mask & (1u << s.bit)) != 0;
    return true;
}

bool ReadDisabled(void* p, bool& disabled) {
    if (!p || !g_resolved.load(std::memory_order_acquire)) return false;
    disabled = BoolAt(p, g_disabledOff);
    return true;
}

bool WriteDisabled(void* p, bool disabled) {
    if (!p || !g_resolved.load(std::memory_order_acquire)) return false;
    BoolAt(p, g_disabledOff) = disabled;
    return true;
}

bool ReadWaterlogged(void* p, bool& waterlogged) {
    if (!p || !g_resolved.load(std::memory_order_acquire)) return false;
    waterlogged = BoolAt(p, g_waterloggedOff);
    return true;
}

void* Lever(void* p, int bit) {
    if (!p || bit < 0 || bit > 4 || !g_resolved.load(std::memory_order_acquire) || g_sys[bit].leverOff < 0)
        return nullptr;
    return *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(p) + g_sys[bit].leverOff);
}

bool PressLever(void* p, void* player, int bit) {
    void* lever = Lever(p, bit);
    void* fn = p ? R::FindDispatchFunctionCached(R::ClassOf(p), L"actionOptionIndex") : nullptr;
    if (!lever || !fn || !player) return false;
    const FVector at = engine::GetComponentLocation(lever);
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", p, lever, at) &&
           f.Set<void*>(L"lookAtComponent", lever) && Call(p, f);
}

bool ButtonsVisibility(void* p) { return CallVerb(p, L"buttonsVisibility"); }

bool PlayLeverSound(void* p, bool on) {
    if (!p || !g_resolved.load(std::memory_order_acquire)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(p), L"playSND");
    ParamFrame f(fn);
    return fn && f.valid() && f.Set(L"activated", on) && Call(p, f);
}

bool SetServersActive(void* p, bool on) {
    std::vector<void*> servers;
    if (!g_servers.Read(p, servers)) return false;
    bool ok = true;
    for (void* sv : servers) {
        // Every server is a serverBox_C; its setActive resolves on the first and is cached for the class.
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(sv), L"setActive");
        ParamFrame f(fn);
        ok = fn && f.valid() && f.Set<bool>(L"bNewActive", on) && Call(sv, f) && ok;
    }
    if (void* hum = HumComponent(p)) ok = component_calls::SetActive(hum, on, false) && ok;
    return ok;
}

bool PlayTurnOnCue(void* p) {
    if (!p) return false;
    void* cue = g_turnOnCue.Get();
    if (!cue) {
        cue = R::FindObject(L"stationTurnon", P::name::SoundWaveClass);
        if (!cue) return false;
        g_turnOnCue.Set(cue);
    }
    FVector at{};
    if (!engine::TryGetActorLocation(p, at)) return false;
    engine::PlaySoundAtLocation(p, cue, at, nullptr);  // no attenuation: heard as the game's 2D cue is
    return true;
}

bool ReadLightRoots(void* p, std::vector<void*>& out) { return g_lightRoots.Read(p, out); }

bool ReadBlackoutDoors(void* p, std::vector<void*>& out) { return g_blackoutDoors.Read(p, out); }

bool ReadUnitPower(UnitPower& out) {
    void* gm = world_singleton::Gamemode();
    if (!gm || !g_resolved.load(std::memory_order_acquire) || g_offUsage < 0) return false;
    bool* flags[5] = {&out.calc, &out.downl, &out.coords, &out.play, &out.light};
    for (int i = 0; i < 5; ++i) {
        if (g_usesp[i].off < 0) return false;
        *flags[i] = (*(static_cast<const uint8_t*>(gm) + g_usesp[i].off) & g_usesp[i].mask) != 0;
    }
    out.usage = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(gm) + g_offUsage);
    return true;
}

bool WriteLightPower(bool on) {
    void* gm = world_singleton::Gamemode();
    if (!gm || !g_resolved.load(std::memory_order_acquire) || g_usesp[4].off < 0) return false;
    uint8_t& b = *(static_cast<uint8_t*>(gm) + g_usesp[4].off);
    b = on ? static_cast<uint8_t>(b | g_usesp[4].mask) : static_cast<uint8_t>(b & ~g_usesp[4].mask);
    return true;
}

bool ReadServers(void* p, ServerState& out) {
    std::vector<void*> servers;
    if (!g_servers.Read(p, servers)) return false;
    out = ServerState{};
    for (void* sv : servers) {
        bool on = false;
        if (!ComponentActive(ObjectField(sv, L"server_loop"), on)) continue;
        ++out.total;
        if (on) ++out.active;
    }
    bool hum = false;
    if (ComponentActive(HumComponent(p), hum)) out.hum = hum ? 1 : 0;
    return true;
}

bool CallVirusLockout(void* p) { return CallVerb(p, L"virus_pb"); }

}  // namespace ue_wrap::power_control
