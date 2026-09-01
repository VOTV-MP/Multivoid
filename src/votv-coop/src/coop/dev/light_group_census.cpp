// coop/dev/light_group_census.cpp -- see coop/dev/light_group_census.h.

#include "coop/dev/light_group_census.h"

#include "coop/config/config.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/lightswitch.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace coop::dev::light_group_census {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace LS = ue_wrap::lightswitch;
namespace WI = ue_wrap::world_identity;

// A full walk of GUObjectArray twice per pass is far too expensive to run often; this is
// a diagnostic, so the cadence is seconds, not frames. FindObjectsByClass documents itself
// as "one-shot / low-rate use only (NOT per-frame)".
constexpr uint64_t kDumpEveryTicks = 300;   // full dump cadence (~5 s at 60 Hz)
constexpr uint64_t kScanEveryTicks = 60;    // change-detect cadence (~1 s at 60 Hz)

bool Enabled() {
    static const bool s_enabled = coop::config::ResolveFlag(::coop::config_registry::rows::lightgroup_census);
    return s_enabled;
}

bool     g_installed  = false;
void*    g_rootCls    = nullptr;
void*    g_switchCls  = nullptr;
int32_t  g_isActiveOff = -1;  // trigger_lightRoot_C::isActive -- the group's live on/off
int32_t  g_activeOff   = -1;  // trigger_lightRoot_C::active   -- the ENABLE GATE (distinct property)
int32_t  g_buffOff     = -1;  // trigger_lightRoot_C::buffIsActive -- the saved copy
int32_t  g_swAOff      = -1;  // Alightswitch_C::A -- presentation only
uint64_t g_tick        = 0;

// key -> packed bits, so a CHANGE line can name WHICH bit moved rather than just "differs".
struct GroupBits { int isActive = -1; int active = -1; int buff = -1; };
std::map<std::wstring, GroupBits> g_lastGroup;
std::map<std::wstring, int>       g_lastSwitch;

// Reads are guarded on a RESOLVED offset. A read through a wrong offset is a garbage
// value; there is no fallback here on purpose -- a probe that invents a number is worse
// than a probe that says it could not look.
int ReadBoolAt(void* obj, int32_t off) {
    if (!obj || off < 0) return -1;
    return static_cast<int>(*reinterpret_cast<const uint8_t*>(reinterpret_cast<const char*>(obj) + off) != 0);
}

// Objects of a DEAD world stay in GUObjectArray until the purge, and a departed world's
// lightRoots would otherwise be censused as if they were the live ones -- the exact class
// of confusion world_identity exists to prevent.
bool InCurrentWorld(void* obj) {
    void* const cur = WI::CurrentWorld();
    if (!cur) return true;                 // world unknown -- do not silently drop everything
    void* const w = WI::WorldOf(obj);
    return (w == nullptr) || (w == cur);   // null stamp == not world-scoped
}

}  // namespace

void Install() {
    if (!Enabled() || g_installed) return;
    void* rootCls = R::FindClass(L"trigger_lightRoot_C");
    if (!rootCls) return;  // blueprint not loaded yet -- retry next tick (throttled by the caller)
    // GetKeyString / GetSwitchKeyString return "" until these have run, and this probe REPORTS
    // an empty key as `unkeyed` -- so without them a census could confidently print "every group
    // is unkeyed" as a measurement. It happens to work today only because two channels call them
    // every tick; an instrument must not depend on someone else having warmed it.
    LS::EnsureResolved();
    LS::EnsureSwitchResolved();
    g_rootCls   = rootCls;
    g_switchCls = R::FindClass(L"lightswitch_C");

    // NOTE the near-collision: `isActive` and `active` are two SEPARATE FBoolProperties on
    // this class. FName lookup is case-insensitive, so asking for L"Active" resolves the
    // GATE and L"IsActive" resolves the live state -- naming them apart here is deliberate.
    g_isActiveOff = R::FindPropertyOffset(rootCls, L"IsActive");
    g_activeOff   = R::FindPropertyOffset(rootCls, L"Active");
    g_buffOff     = R::FindPropertyOffset(rootCls, L"buffIsActive");
    if (g_switchCls) g_swAOff = R::FindPropertyOffset(g_switchCls, L"A");

    g_installed = true;
    UE_LOGI("[LGC] installed: trigger_lightRoot_C=%p isActive@0x%X active@0x%X buffIsActive@0x%X ; "
            "lightswitch_C=%p A@0x%X. READ-ONLY -- this probe never writes and never calls a UFunction.",
            rootCls, g_isActiveOff, g_activeOff, g_buffOff, g_switchCls, g_swAOff);
    if (g_isActiveOff < 0 || g_activeOff < 0)
        UE_LOGW("[LGC] isActive/active did NOT both resolve -- the census cannot answer the gate question");
}

void Tick() {
    if (!Enabled()) return;
    // Throttle FIRST. Install() opens with R::FindClass, which on a MISS is a full
    // GUObjectArray walk that is deliberately not cached -- retrying it every frame until the
    // blueprint loads is the per-frame-walk pattern this project has already paid for once.
    if ((++g_tick % kScanEveryTicks) != 0) return;
    if (!g_installed) { Install(); return; }
    const bool fullDump = (g_tick % kDumpEveryTicks) == 0;

    UE_ASSERT_GAME_THREAD("light_group_census::Tick");

    // --- light GROUPS: the state a player sees, plus the gate that decides whether a
    // press can move it, plus the saved copy the next load will restore from.
    std::vector<void*> roots = R::FindObjectsByClass(L"trigger_lightRoot_C");
    size_t keyed = 0, unkeyed = 0;
    for (void* r : roots) {
        if (!r || !R::IsLive(r) || !InCurrentWorld(r)) continue;
        const std::wstring key = LS::GetKeyString(r);
        if (key.empty() || key == L"None") { ++unkeyed; continue; }  // counted, not silently dropped
        ++keyed;
        GroupBits now;
        now.isActive = ReadBoolAt(r, g_isActiveOff);
        now.active   = ReadBoolAt(r, g_activeOff);
        now.buff     = ReadBoolAt(r, g_buffOff);

        auto it = g_lastGroup.find(key);
        if (it == g_lastGroup.end()) {
            g_lastGroup[key] = now;
            UE_LOGI("[LGC] seen  group key='%ls' isActive=%d active=%d buffIsActive=%d",
                    key.c_str(), now.isActive, now.active, now.buff);
        } else if (it->second.isActive != now.isActive || it->second.active != now.active ||
                   it->second.buff != now.buff) {
            UE_LOGI("[LGC] CHANGE group key='%ls' isActive %d->%d active %d->%d buffIsActive %d->%d",
                    key.c_str(), it->second.isActive, now.isActive, it->second.active, now.active,
                    it->second.buff, now.buff);
            it->second = now;
        }
        if (fullDump)
            UE_LOGI("[LGC] dump  group key='%ls' isActive=%d active=%d buffIsActive=%d",
                    key.c_str(), now.isActive, now.active, now.buff);
    }

    // --- SWITCHES: the presentation bit the shipped lane already syncs. Logged beside the
    // groups so a two-peer diff can tell "the switches agree but the lights do not" -- the
    // signature of a divergence the shipped poll is structurally blind to.
    if (g_switchCls && g_swAOff >= 0) {
        std::vector<void*> switches = R::FindObjectsByClass(L"lightswitch_C");
        for (void* s : switches) {
            if (!s || !R::IsLive(s) || !InCurrentWorld(s)) continue;
            const std::wstring key = LS::GetSwitchKeyString(s);
            if (key.empty() || key == L"None") continue;
            const int a = ReadBoolAt(s, g_swAOff);
            auto it = g_lastSwitch.find(key);
            if (it == g_lastSwitch.end()) {
                g_lastSwitch[key] = a;
                UE_LOGI("[LGC] seen  switch key='%ls' A=%d", key.c_str(), a);
            } else if (it->second != a) {
                UE_LOGI("[LGC] CHANGE switch key='%ls' A %d->%d", key.c_str(), it->second, a);
                it->second = a;
            }
            if (fullDump) UE_LOGI("[LGC] dump  switch key='%ls' A=%d", key.c_str(), a);
        }
    }

    if (fullDump)
        UE_LOGI("[LGC] dump  END groups keyed=%zu unkeyed=%zu (unkeyed groups are invisible to any "
                "Key-identified sync lane -- that is a measurement, not a filter)", keyed, unkeyed);
}

}  // namespace coop::dev::light_group_census
