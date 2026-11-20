// coop/player/level_visit_sky.cpp -- see coop/player/level_visit_sky.h.

#include "coop/player/level_visit_sky.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <string>
#include <vector>

namespace coop::level_visit_sky {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace UH = ue_wrap::ufunction_hook;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowSetVisibility, "level_visit_sky", L"SceneComponent", L"SetVisibility", Declared);

// The world's sky: what lights or covers a level without a bound on its reach. A map travelled to is a world of its own
// with none of the world's; loaded in place, 600 m below, it would have all of it. The height fog's density grows
// without bound below its height and covers the map in its colour; the sun and the sky light light a maze the game
// leaves dark but for the player's lantern; the world's unbound post process sets the view's exposure and grading.
// The components are hidden and the volumes switched off for the visit on this machine, and put back after.
constexpr const wchar_t* kWorldSkyClasses[] = {L"ExponentialHeightFogComponent", L"DirectionalLightComponent",
                                               L"SkyLightComponent", L"SkyAtmosphereComponent",
                                               L"VolumetricCloudComponent"};
constexpr const wchar_t* kMapLightClasses[] = {L"PointLightComponent", L"SpotLightComponent", L"RectLightComponent",
                                               L"DirectionalLightComponent", L"SkyLightComponent",
                                               L"ExponentialHeightFogComponent"};

// A held component: the value the world wants it to have at the visit's end. It starts as the value it had when the visit
// began and takes the last value the game's own scripts (the day-night cycle) asked for while the visit held it.
struct Held {
    void* comp;
    bool before;   // shown when the visit began
    bool want;     // shown at the visit's end
};
std::vector<Held> g_held;
std::vector<void*> g_offVolumes;
std::vector<SkyEnd> g_lastEnd;
int g_lastUnreadable = 0;

// The SetVisibility pre hook: patched once, armed only while a visit holds the sky (a native every Blueprint calls).
void* g_setVisFn = nullptr;
int32_t g_offNewVisibility = -1;
bool g_hookInstalled = false;
bool g_hookRefused = false;
bool g_ownWrite = false;   // this file's own SetVisibility call is in flight: it passes

// A scene component's own bVisible; false when it is gone or does not read.
bool ReadVisible(void* comp, bool* shown) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!comp || !R::IsLive(comp) || !R::FindBoolProperty(R::ClassOf(comp), L"bVisible", off, mask) || off < 0) return false;
    *shown = (static_cast<const uint8_t*>(comp)[off] & mask) != 0;
    return true;
}

bool SetComponentVisible(void* comp, bool on) {
    void* fn = comp && R::IsLive(comp) ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"SetVisibility") : nullptr;
    ue_wrap::ParamFrame pf(fn);
    if (!fn || !pf.valid() || !pf.Set<bool>(L"bNewVisibility", on) || !pf.Set<bool>(L"bPropagateToChildren", false)) return false;
    g_ownWrite = true;
    const bool ran = ue_wrap::Call(comp, pf);
    g_ownWrite = false;
    return ran;
}

// A write of the game's own on a held component is refused and its value kept for the visit's end. Held components are
// a handful: a linear scan.
sg::Verdict OnSetVisibilityPre(void* context, void* /*sourceObject*/, const uint8_t* parms) {
    if (g_ownWrite || !parms || g_offNewVisibility < 0) return sg::Verdict::Run;
    for (Held& h : g_held) {
        if (h.comp != context) continue;
        h.want = parms[g_offNewVisibility] != 0;
        return sg::Verdict::Cancel;
    }
    return sg::Verdict::Run;
}

// Patches SetVisibility disarmed, once; false when it cannot be (said once), and the visit then holds nothing against the
// day-night cycle.
bool EnsureHook() {
    if (g_hookInstalled) return true;
    if (g_hookRefused) return false;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowSetVisibility);
    if (!fn) return false;   // not loaded yet: the next visit tries again
    g_setVisFn = fn.Fn();
    g_offNewVisibility = R::FindParamOffset(g_setVisFn, L"bNewVisibility");
    if (g_offNewVisibility < 0 || !UH::InstallPreHook(fn, &OnSetVisibilityPre, /*armed=*/false)) {
        g_hookRefused = true;
        UE_LOGE("level_visit: no pre hook on SetVisibility (offset %d) -- the day-night cycle's writes are not held in a "
                "visit", g_offNewVisibility);
        return false;
    }
    g_hookInstalled = true;
    return true;
}

// Every live instance of the class named, split by whether its level (a component's actor's, or an actor's own) is
// the visited one.
void InstancesOf(const wchar_t* name, bool component, void* mapLevel, std::vector<void*>& world,
                 std::vector<void*>& map) {
    void* cls = OI::ClassByName(name);
    if (!cls) return;
    struct Ctx {
        bool component;
        void* mapLevel;
        std::vector<void*>* world;
        std::vector<void*>* map;
    } c{component, mapLevel, &world, &map};
    OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
        auto* k = static_cast<Ctx*>(ctx);
        if (!R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__")) return;
        void* level = k->component ? R::OuterOf(R::OuterOf(o)) : R::OuterOf(o);
        (level == k->mapLevel ? k->map : k->world)->push_back(o);
    }, &c);
}

// A post process volume's bitfield bool, read or written.
bool VolumeBit(void* v, const wchar_t* name, int set = -1) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(v), name, off, mask) || off < 0) return false;
    uint8_t& b = *(static_cast<uint8_t*>(v) + off);
    if (set >= 0) b = set ? (b | mask) : (b & ~mask);
    return (b & mask) != 0;
}

}  // namespace

void Hide(void* mapLevel) {
    std::wstring said, map;
    g_held.clear();
    g_lastEnd.clear();
    g_lastUnreadable = 0;
    for (const wchar_t* name : kWorldSkyClasses) {
        std::vector<void*> world, inMap;
        InstancesOf(name, true, mapLevel, world, inMap);
        int hidden = 0;
        // Every world sky component is held, shown or not: the day-night cycle turns some on and some off as the visit
        // runs, and what it asks is applied at the end. Only a shown one is hidden now.
        for (void* c : world) {
            bool shown = false;
            if (!ReadVisible(c, &shown)) { ++g_lastUnreadable; continue; }
            bool dup = false;
            for (const Held& h : g_held) dup = dup || h.comp == c;
            if (dup) continue;
            g_held.push_back({c, shown, shown});
            if (shown && SetComponentVisible(c, false)) ++hidden;
        }
        if (!world.empty()) said += L" " + std::wstring(name) + L"=" + std::to_wstring(hidden);
    }
    // The held components' own writes are the day-night cycle's to ask, and are asked of the end: armed only now.
    if (!g_held.empty() && EnsureHook()) UH::SetArmed(g_setVisFn, &OnSetVisibilityPre, true);
    std::vector<void*> volumes, mapVolumes;
    InstancesOf(L"PostProcessVolume", false, mapLevel, volumes, mapVolumes);
    for (void* v : volumes)
        if (VolumeBit(v, L"bEnabled")) { VolumeBit(v, L"bEnabled", 0); g_offVolumes.push_back(v); }
    for (const wchar_t* name : kMapLightClasses) {
        std::vector<void*> world, inMap;
        InstancesOf(name, true, mapLevel, world, inMap);
        if (!inMap.empty()) map += L" " + std::wstring(name) + L"=" + std::to_wstring(inMap.size());
    }
    int unbound = 0;
    for (void* v : mapVolumes) unbound += VolumeBit(v, L"bUnbound") ? 1 : 0;
    UE_LOGI("level_visit: the world's sky held for the visit (%zu components, %d unreadable, hook %s):%ls "
            "PostProcessVolume=%zu; the map's own:%ls PostProcessVolume=%zu (unbound %d)", g_held.size(), g_lastUnreadable,
            g_hookInstalled ? "armed" : "absent", said.c_str(), g_offVolumes.size(),
            map.empty() ? L" no light" : map.c_str(), mapVolumes.size(), unbound);
}

void Show() {
    if (g_hookInstalled) UH::SetArmed(g_setVisFn, &OnSetVisibilityPre, false);
    // [dev red] visit_sky_all: every held component is shown at the end, as before the visit kept what it found.
    static const bool all = coop::config::ResolveFlag(coop::config_registry::rows::visit_sky_all);
    g_lastEnd.clear();
    for (const Held& h : g_held) {
        g_lastEnd.push_back({h.comp, h.before, h.want});
        bool now = false;
        const bool target = all ? true : h.want;
        if (R::IsLive(h.comp) && ReadVisible(h.comp, &now) && now != target) SetComponentVisible(h.comp, target);
    }
    for (void* v : g_offVolumes)
        if (R::IsLive(v)) VolumeBit(v, L"bEnabled", 1);
    g_held.clear();
    g_offVolumes.clear();
}

const std::vector<SkyEnd>& LastVisitEnd() { return g_lastEnd; }

int LastVisitUnreadable() { return g_lastUnreadable; }

bool ReadShown(void* comp, bool* shown) { return ReadVisible(comp, shown); }

}  // namespace coop::level_visit_sky
