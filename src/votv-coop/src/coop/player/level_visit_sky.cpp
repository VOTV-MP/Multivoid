// coop/player/level_visit_sky.cpp -- see coop/player/level_visit_sky.h.

#include "coop/player/level_visit_sky.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <string>
#include <vector>

namespace coop::level_visit_sky {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

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
std::vector<void*> g_hiddenSky;
std::vector<void*> g_offVolumes;
ULONGLONG g_skyHeldAtMs = 0;

bool SetComponentVisible(void* comp, bool on) {
    void* fn = comp && R::IsLive(comp) ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"SetVisibility") : nullptr;
    ue_wrap::ParamFrame pf(fn);
    return fn && pf.valid() && pf.Set<bool>(L"bNewVisibility", on) && pf.Set<bool>(L"bPropagateToChildren", false) &&
           ue_wrap::Call(comp, pf);
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
    for (const wchar_t* name : kWorldSkyClasses) {
        std::vector<void*> world, inMap;
        InstancesOf(name, true, mapLevel, world, inMap);
        int hidden = 0;
        for (void* c : world)
            if (SetComponentVisible(c, false)) { g_hiddenSky.push_back(c); ++hidden; }
        if (!world.empty()) said += L" " + std::wstring(name) + L"=" + std::to_wstring(hidden);
    }
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
    g_skyHeldAtMs = ::GetTickCount64();
    UE_LOGI("level_visit: the world's sky hidden for the visit:%ls PostProcessVolume=%zu; the map's own:%ls "
            "PostProcessVolume=%zu (unbound %d)", said.c_str(), g_offVolumes.size(), map.empty() ? L" no light" : map.c_str(),
            mapVolumes.size(), unbound);
}

// The day-night cycle sets the sun and the sky as it runs; what it shows again is hidden again, once a second.
void Hold() {
    if (::GetTickCount64() - g_skyHeldAtMs < 1000) return;
    g_skyHeldAtMs = ::GetTickCount64();
    for (void* c : g_hiddenSky)
        if (R::IsLive(c)) SetComponentVisible(c, false);
    for (void* v : g_offVolumes)
        if (R::IsLive(v) && VolumeBit(v, L"bEnabled")) VolumeBit(v, L"bEnabled", 0);
}

void Show() {
    for (void* c : g_hiddenSky)
        if (R::IsLive(c)) SetComponentVisible(c, true);
    for (void* v : g_offVolumes)
        if (R::IsLive(v)) VolumeBit(v, L"bEnabled", 1);
    g_hiddenSky.clear();
    g_offVolumes.clear();
}

}  // namespace coop::level_visit_sky