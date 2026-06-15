// coop/dev/event_trigger.cpp -- see coop/dev/event_trigger.h.

#include "coop/dev/event_trigger.h"

#include "coop/dev/dev_gate.h"
#include "ue_wrap/call.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <chrono>
#include <string>

namespace coop::dev::event_trigger {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

void* g_gmCls = nullptr;
void* g_gm = nullptr;
int32_t g_gmIdx = -1;
int32_t g_offEventer = -1;        // mainGamemode.eventer (trigger_eventer_C*)
void* g_eventerCls = nullptr;
void* g_runEventFn = nullptr;     // runEvent(FName event, FName special)
std::chrono::steady_clock::time_point g_nextResolve{};
bool g_resolved = false;

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gmCls) g_gmCls = R::FindClass(L"mainGamemode_C");
    if (!g_eventerCls) g_eventerCls = R::FindClass(L"trigger_eventer_C");
    if (!g_gmCls || !g_eventerCls) return;
    if (g_offEventer < 0) g_offEventer = R::FindPropertyOffset(g_gmCls, L"eventer");
    if (!g_runEventFn) g_runEventFn = R::FindFunction(g_eventerCls, L"runEvent");
    if (!g_resolved && g_offEventer >= 0 && g_runEventFn) {
        g_resolved = true;
        UE_LOGI("event_trigger: resolved (eventer off=0x%X runEvent=yes, %zu menu entries)",
                g_offEventer, Events().size());
    }
}

void* Eventer() {
    if (!g_gm || !R::IsLiveByIndex(g_gm, g_gmIdx)) {
        g_gm = nullptr;
        if (!g_gmCls) return nullptr;
        for (void* obj : R::FindObjectsByClass(L"mainGamemode_C")) {
            if (obj && R::IsLive(obj)) {
                g_gm = obj;
                g_gmIdx = R::InternalIndexOf(obj);
                break;
            }
        }
    }
    if (!g_gm || g_offEventer < 0) return nullptr;
    void* ev = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_gm) + g_offEventer);
    return (ev && R::IsLive(ev)) ? ev : nullptr;
}

}  // namespace

const std::vector<EventInfo>& Events() {
    // The 65 SwitchName cases (votv-event-system-RE-2026-06-13.md section 1.2)
    // + the ariralPrank special. dayZ = the DataTable unlock day (-1 = story/
    // trigger-only). Risk per the doc's section-5 safety table.
    static const std::vector<EventInfo> kEvents = {
        { "starRain",        1, "sky/visual",   Risk::Safe,      false },
        { "solar",           3, "sky/visual",   Risk::Safe,      false },
        { "wisps",          13, "sky/visual",   Risk::Safe,      false },
        { "peace",           5, "ambient",      Risk::Safe,      false },
        { "picnic",          7, "world prop",   Risk::Safe,      false },
        { "destroyPicnic",   9, "world prop",   Risk::Safe,      false },
        { "salt",           24, "prop",         Risk::Safe,      false },
        { "cookier",        24, "npc",          Risk::Safe,      false },
        { "enasus",         14, "prop drop",    Risk::Safe,      false },
        { "enacros",        19, "prop drop",    Risk::Safe,      false },
        { "ariralPrank",    -1, "ariral prank", Risk::Safe,      true  },
        { "arirShip",       12, "ariral",       Risk::Safe,      false },
        { "arirSignal",      8, "signal",       Risk::Safe,      false },
        { "picSignal",      11, "signal",       Risk::Safe,      false },
        { "arirSpk",        10, "ariral",       Risk::Safe,      false },
        { "arirFollower",   -1, "ariral npc",   Risk::Safe,      false },
        { "arirEgg",        -1, "ariral",       Risk::Safe,      false },
        { "susArir",        -1, "ariral",       Risk::Safe,      false },
        { "arirSat_0",      25, "signal/sat",   Risk::Safe,      false },
        { "arirSat_1",      25, "signal/sat",   Risk::Safe,      false },
        { "arirSat_2",      25, "signal/sat",   Risk::Safe,      false },
        { "soltoClean",     34, "ariral npc",   Risk::Safe,      false },
        { "morningGay",     36, "ufo",          Risk::Safe,      false },
        { "falseEnter",     -1, "scare",        Risk::Safe,      false },
        { "mann",           -1, "scare",        Risk::Safe,      false },
        { "vent",           -1, "scare",        Risk::Safe,      false },
        { "fakeGrays",      -1, "scare",        Risk::Safe,      false },
        { "toeStab",        23, "scare",        Risk::Safe,      false },
        { "ventKnocker",    11, "scare",        Risk::Safe,      false },
        { "vehtp",          -1, "world",        Risk::Safe,      false },
        { "crys",           -1, "world",        Risk::Safe,      false },
        { "paperGray",      -1, "prop",         Risk::Safe,      false },
        { "agrav",          16, "physics",      Risk::Caution,   false },  // RE'd 2026-06-13 (trigger_agrav kismet, full 443-name symbol table): physics-ONLY -- SetEnableGravity(false)+SetPhysicsLinearVelocity/Angular on props in Bounds + a camera post-process cloak. NO travel/menu/quit/teleport/damage/death/kill verb -> agrav CANNOT itself end the session. Caution (not Safe) because it flings loose physics props and mainPlayer has impact-damage entry points (receivedPhyiscsDamage/impactDamage), so a flung heavy prop can hurt the host. The round-4 menu-travel (~84s later) was loadLevel('menu') from mainPlayer's death/recovery cluster -- NOT proven agrav-caused (no vitals/input trace; see votv-event-system-RE doc + the loadLevel probe).
        { "looker_0-1",     27, "looker npc",   Risk::Caution,   false },
        { "looker_1-1",     27, "looker npc",   Risk::Caution,   false },
        { "looker_2-1",     28, "looker npc",   Risk::Caution,   false },
        { "looker_3-1",     28, "looker npc",   Risk::Caution,   false },
        { "looker_4-1",     28, "looker npc",   Risk::Caution,   false },
        { "ventCrawler",     4, "creature",     Risk::Caution,   false },
        { "tentacleBalls",  32, "creature",     Risk::Caution,   false },
        { "fallbody_0",     39, "ufo drop",     Risk::Caution,   false },
        { "fallbody_1",     40, "ufo drop",     Risk::Caution,   false },
        { "fallcar_0",      41, "ufo drop",     Risk::Caution,   false },
        { "arirBuster",     14, "invasion",     Risk::Caution,   false },
        { "graysforest",    42, "invasion",     Risk::Caution,   false },
        { "graystank",      43, "invasion",     Risk::Caution,   false },
        { "eggvasion",      -1, "invasion",     Risk::Caution,   false },
        { "boarwar",        46, "invasion",     Risk::Caution,   false },
        { "bedEvent",       -1, "sleep/dream",  Risk::Caution,   false },
        { "earthTp",        26, "TELEPORT",     Risk::Dangerous, false },
        { "treehouse_0",    16, "story build",  Risk::Dangerous, false },
        { "treehouse_1",    17, "story build",  Risk::Dangerous, false },
        { "treehouse_2",    18, "story build",  Risk::Dangerous, false },
        { "treehouse_3",    19, "story build",  Risk::Dangerous, false },
        { "treehouse_4",    20, "story build",  Risk::Dangerous, false },
        { "treehouse_5",    21, "story build",  Risk::Dangerous, false },
        { "break_Victor",   16, "story",        Risk::Dangerous, false },
        { "break_Victor2",  19, "story",        Risk::Dangerous, false },
        { "break_RomeoSierra", 7, "story",      Risk::Dangerous, false },
        { "obelisk",        24, "story struct", Risk::Dangerous, false },
        { "piramid",        30, "story struct", Risk::Dangerous, false },
        { "piramid_sig",    29, "signal/story", Risk::Dangerous, false },
        { "borgRozital",    37, "story npc",    Risk::Dangerous, false },
        { "rozitalHole",    38, "world/story",  Risk::Dangerous, false },
        { "dreambase",      30, "dream/story",  Risk::Dangerous, false },
        { "call0",          47, "story (end)",  Risk::Dangerous, false },
    };
    return kEvents;
}

bool Trigger(const EventInfo& ev) {
    if (!::coop::dev_gate::Allowed()) {
        UE_LOGW("event_trigger: REFUSED -- dev features are disabled while connected as a client");
        return false;
    }
    ResolvePass();
    if (!g_resolved) {
        UE_LOGW("event_trigger: not resolved yet (gamemode/eventer/runEvent pending)");
        return false;
    }
    // Copy the POD info -- the GT task may outlive this render-thread frame.
    const std::string name = ev.name;
    const bool prank = ev.prank;
    GT::Post([name, prank] {
        void* eventer = Eventer();
        if (!eventer) {
            UE_LOGW("event_trigger: no live trigger_eventer (world not up?)");
            return;
        }
        ue_wrap::ParamFrame f(g_runEventFn);
        if (!f.valid()) return;
        const std::wstring wname(name.begin(), name.end());
        // The prank branch keys on `special`; `event` is ignored there (any
        // arirInteraction row name works -- the doc's section 2.4).
        const std::wstring eventName = prank ? L"arirInteraction_0" : wname;
        const std::wstring specialName = prank ? L"ariralPrank" : L"None";
        f.Set<R::FName>(L"event", ue_wrap::fname_utils::StringToFName(eventName));
        f.Set<R::FName>(L"special", ue_wrap::fname_utils::StringToFName(specialName));
        if (ue_wrap::Call(eventer, f))
            UE_LOGI("event_trigger: runEvent('%s'%s) dispatched", name.c_str(),
                    prank ? ", special=ariralPrank" : "");
        else
            UE_LOGW("event_trigger: runEvent('%s') dispatch FAILED", name.c_str());
    });
    return true;
}

}  // namespace coop::dev::event_trigger
