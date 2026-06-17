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
int32_t g_offEventer = -1;          // mainGamemode.eventer (trigger_eventer_C*)
void* g_eventerCls = nullptr;
void* g_runEventFn = nullptr;        // runEvent(FName Event, FName Special)
void* g_runSpecialEventFn = nullptr; // runSpecialEvent(FName eventName1) -> bool  (the per-name prank switch)
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
    if (!g_runSpecialEventFn) g_runSpecialEventFn = R::FindFunction(g_eventerCls, L"runSpecialEvent");
    if (!g_resolved && g_offEventer >= 0 && g_runEventFn) {
        g_resolved = true;
        UE_LOGI("event_trigger: resolved (eventer off=0x%X runEvent=yes runSpecialEvent=%s, %zu menu entries)",
                g_offEventer, g_runSpecialEventFn ? "yes" : "no", Events().size());
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

const char* CategoryName(Category c) {
    switch (c) {
        case Category::Story:    return "Story";
        case Category::Ariral:   return "Ariral";
        case Category::Creature: return "Creature";
        case Category::Signal:   return "Signal";
        case Category::Prop:     return "Prop";
        case Category::Cosmetic: return "Cosmetic";
        case Category::Sound:    return "Sound";
        case Category::Scare:    return "Scare";
        case Category::Teleport: return "Teleport";
        case Category::Physics:  return "Physics";
        case Category::Dream:    return "Dream";
        case Category::World:    return "World";
        default:                 return "Other";
    }
}

const std::vector<EventInfo>& Events() {
    using D = Dispatch;
    using C = Category;
    // ALL natively-triggerable events across BOTH dispatchers (runEvent 65 + runSpecialEvent 34
    // effective), ordered BY CATEGORY then day. dayZ = list_events DataTable time.Z (-1 = trigger/
    // rep-gated). `time` = native trigger time-of-day (time.X:time.Y anchor / "trigger" build-or-story /
    // "rep-gated" prank pool). Sources: votv-event-system-RE-2026-06-13.md (runEvent cases + safety),
    // the 2026-06-17 dispatcher sweep (runSpecialEvent 36-case switch + daynightCycle X=Hour/Y=Min/Z=Day),
    // list_events DataTable (day + HH:MM), votv-all-events-coop-sync-classification-2026-06-17.md (names).
    static const std::vector<EventInfo> kEvents = {
        // ---- Story (narrative / save progression) ----
        { "break_RomeoSierra", D::RunEvent, C::Story, 7,  "01:44", "breaks the Romeo/Sierra servers",        Risk::Dangerous },
        { "arirShip",          D::RunEvent, C::Story, 12, "00:01", "arms arirShip_C (radio-tower ariral ship)", Risk::Caution },
        { "treehouse_0",       D::RunEvent, C::Story, 16, "trigger","ariral treehouse build stage 0",         Risk::Dangerous },
        { "break_Victor",      D::RunEvent, C::Story, 16, "01:11", "breaks the Victor servers",               Risk::Dangerous },
        { "treehouse_1",       D::RunEvent, C::Story, 17, "trigger","treehouse build stage 1",                Risk::Dangerous },
        { "treehouse_2",       D::RunEvent, C::Story, 18, "trigger","treehouse build stage 2",                Risk::Dangerous },
        { "treehouse_3",       D::RunEvent, C::Story, 19, "trigger","treehouse build stage 3",                Risk::Dangerous },
        { "break_Victor2",     D::RunEvent, C::Story, 19, "01:02", "2nd Victor server break",                 Risk::Dangerous },
        { "treehouse_4",       D::RunEvent, C::Story, 20, "trigger","treehouse build stage 4",                Risk::Dangerous },
        { "treehouse_5",       D::RunEvent, C::Story, 21, "trigger","treehouse build stage 5 (final)",        Risk::Dangerous },
        { "obelisk",           D::RunEvent, C::Story, 24, "15:40", "arms the warning obelisk + alarms",       Risk::Dangerous },
        { "looker_0-1",        D::RunEvent, C::Story, 27, "20:00", "forceObjects += looker (sky)",            Risk::Caution },
        { "looker_1-1",        D::RunEvent, C::Story, 27, "22:00", "forceObjects += looker (window)",         Risk::Caution },
        { "looker_2-1",        D::RunEvent, C::Story, 28, "00:00", "forceObjects += looker",                  Risk::Caution },
        { "looker_3-1",        D::RunEvent, C::Story, 28, "02:00", "forceObjects += looker",                  Risk::Caution },
        { "looker_4-1",        D::RunEvent, C::Story, 28, "04:00", "forceObjects += looker",                  Risk::Caution },
        { "piramid",           D::RunEvent, C::Story, 30, "18:00", "arms the piramid structure",              Risk::Dangerous },
        { "dreambase",         D::RunEvent, C::Story, 30, "18:00", "spawns the dreambase + pivots",           Risk::Dangerous },
        { "borgRozital",       D::RunEvent, C::Story, 37, "11:00", "spawns the rozitBorg mothership",         Risk::Dangerous },
        { "rozitalHole",       D::RunEvent, C::Story, 38, "00:00", "bottom-hole controller",                  Risk::Dangerous },
        { "call0",             D::RunEvent, C::Story, 47, "00:01", "day-47 END (roar; alphaFinish gate)",     Risk::Dangerous },

        // ---- Ariral (faction NPCs / reputation interactions + pranks) ----
        { "arirBuster",   D::RunEvent,     C::Ariral, 14, "17:00",   "spawns arirBusterSpawner_C",            Risk::Caution },
        { "soltoClean",   D::RunEvent,     C::Ariral, 34, "19:00",   "spawns soltomiaCleaning_C",             Risk::Caution },
        { "arirFollower", D::RunEvent,     C::Ariral, -1, "trigger", "spawns npc_arirFollower_C",             Risk::Caution },
        { "susArir",      D::RunEvent,     C::Ariral, -1, "trigger", "ariral 'sus' interaction (per-player)", Risk::Caution },
        { "ariralPrank",  D::RandomPrank,  C::Ariral, -1, "rep-gated","RANDOM rep-tiered ariral prank (summonArirPrank)", Risk::Caution },
        { "food",         D::SpecialEvent, C::Ariral, -1, "rep-gated","ariral throws a food prop (good tier)", Risk::Safe },
        { "drive",        D::SpecialEvent, C::Ariral, -1, "rep-gated","ariral throws a drive prop (good tier)", Risk::Safe },
        { "atvFuel",      D::SpecialEvent, C::Ariral, -1, "rep-gated","ariral refuels the ATV",               Risk::Safe },
        { "atvFix",       D::SpecialEvent, C::Ariral, -1, "rep-gated","ariral ATV repair (toolbox)",          Risk::Safe },
        { "poisonFood",   D::SpecialEvent, C::Ariral, -1, "rep-gated","ariral throws SPIKED food (bad tier)", Risk::Caution },
        { "expDrive",     D::SpecialEvent, C::Ariral, -1, "rep-gated","ariral throws an EXPLOSIVE drive (bad tier)", Risk::Dangerous },

        // ---- Creature (hostile/ambient spawns + invasions) ----
        { "ventCrawler",  D::RunEvent,     C::Creature, 4,  "23:27",   "spawns ventCrawler_C",               Risk::Caution },
        { "ventKnocker",  D::RunEvent,     C::Creature, 11, "00:38",   "spawns kocker_C (vent banging)",      Risk::Caution },
        { "wisps",        D::RunEvent,     C::Creature, 13, "19:17",   "arms a wisp_C swarm",                 Risk::Caution },
        { "tentacleBalls",D::RunEvent,     C::Creature, 32, "18:00",   "spawns tentacleBallsFollower_C",      Risk::Caution },
        { "morningGay",   D::RunEvent,     C::Creature, 36, "10:50",   "spawns morningUfo_C",                 Risk::Caution },
        { "fallbody_0",   D::RunEvent,     C::Creature, 39, "23:11",   "UFO drops a body (ufoDropper_body_C)", Risk::Caution },
        { "fallbody_1",   D::RunEvent,     C::Creature, 40, "04:18",   "UFO drops a body (window variant)",   Risk::Caution },
        { "fallcar_0",    D::RunEvent,     C::Creature, 41, "22:28",   "UFO drops a car (ufoDropper_car_C)",  Risk::Caution },
        { "graysforest",  D::RunEvent,     C::Creature, 42, "17:01",   "gray forest invasion controller",    Risk::Dangerous },
        { "graystank",    D::RunEvent,     C::Creature, 43, "18:01",   "UFO drops a tank + pig",             Risk::Dangerous },
        { "boarwar",      D::RunEvent,     C::Creature, 46, "03:00",   "boar invasion (90 min)",             Risk::Dangerous },
        { "eggvasion",    D::RunEvent,     C::Creature, -1, "trigger", "superEgger_C invasion",              Risk::Dangerous },
        { "trashBase",    D::SpecialEvent, C::Creature, -1, "rep-gated","spawns arirTrasher_C",              Risk::Caution },
        { "rockThrow",    D::SpecialEvent, C::Creature, -1, "rep-gated","rockThrower_C x3 (rocks at window)", Risk::Caution },
        { "hillRoller",   D::SpecialEvent, C::Creature, -1, "rep-gated","spawns hillRollerSpawner_C",        Risk::Caution },
        { "alienJump",    D::SpecialEvent, C::Creature, -1, "rep-gated","spawns alienJump_C",                Risk::Caution },

        // ---- Signal (SETI / forceObjects beats) ----
        { "peace",     D::RunEvent, C::Signal, 5,  "23:14", "the 'peace' signal (forceObjects)",      Risk::Safe },
        { "arirSignal",D::RunEvent, C::Signal, 8,  "18:00", "ariral signal (forceObjects)",           Risk::Safe },
        { "arirSpk",   D::RunEvent, C::Signal, 10, "00:02", "ariral speak message (forceObjects)",    Risk::Safe },
        { "picSignal", D::RunEvent, C::Signal, 11, "23:10", "picnic signal (forceObjects)",           Risk::Safe },
        { "arirSat_0", D::RunEvent, C::Signal, 25, "11:00", "ariral satellite signal",                Risk::Safe },
        { "arirSat_1", D::RunEvent, C::Signal, 25, "12:00", "ariral satellite signal",                Risk::Safe },
        { "arirSat_2", D::RunEvent, C::Signal, 25, "13:00", "ariral satellite signal",                Risk::Safe },
        { "piramid_sig",D::RunEvent,C::Signal, 29, "18:00", "piramid signal (forceObjects)",          Risk::Safe },

        // ---- Prop (world prop spawn/remove) ----
        { "picnic",       D::RunEvent,     C::Prop, 7,  "00:44",   "places the picnic props",          Risk::Safe },
        { "destroyPicnic",D::RunEvent,     C::Prop, 9,  "00:00",   "removes the picnic props",         Risk::Safe },
        { "enasus",       D::RunEvent,     C::Prop, 14, "08:49",   "drops sushi + the enasus note",    Risk::Safe },
        { "enacros",      D::RunEvent,     C::Prop, 19, "08:49",   "drops a croissant + the enacros note", Risk::Safe },
        { "salt",         D::RunEvent,     C::Prop, 24, "18:00",   "spawns saltpile_C (salt heart)",   Risk::Safe },
        { "cookier",      D::RunEvent,     C::Prop, 24, "05:30",   "arms prop_cookiebox_C",            Risk::Safe },
        { "arirEgg",      D::RunEvent,     C::Prop, -1, "trigger",  "arms prop_arirEgg_C",             Risk::Safe },
        { "paperGray",    D::RunEvent,     C::Prop, -1, "trigger",  "arms a paper-gray prop scare",    Risk::Safe },
        { "cookiebox",    D::SpecialEvent, C::Prop, -1, "rep-gated","spawns prop_cookiebox_C (gift)",  Risk::Safe },
        { "trashPiles",   D::SpecialEvent, C::Prop, -1, "rep-gated","trash props at doorways",         Risk::Safe },
        { "arirGraff",    D::SpecialEvent, C::Prop, -1, "rep-gated","graffiti decal (grime_arirGraffiti_C)", Risk::Safe },
        { "vaccine",      D::SpecialEvent, C::Prop, -1, "rep-gated","spawns event_vaccine_C",          Risk::Caution },
        { "oil",          D::SpecialEvent, C::Prop, -1, "rep-gated","spawns oilStainSpawn_C x8",       Risk::Caution },
        { "begos",        D::SpecialEvent, C::Prop, -1, "rep-gated","spawns begoExplosion_C x4",       Risk::Dangerous },
        { "gascans",      D::SpecialEvent, C::Prop, -1, "rep-gated","explosive gascans (event_funnyGascans_C)", Risk::Dangerous },
        { "bombBox",      D::SpecialEvent, C::Prop, -1, "rep-gated","spawns prop_bombbox_C",           Risk::Dangerous },

        // ---- Cosmetic (sky / particle visuals) ----
        { "starRain", D::RunEvent, C::Cosmetic, 1, "00:17", "shooting-star/meteor emitter (eff_shootingStar_rain)", Risk::Safe },
        { "solar",    D::RunEvent, C::Cosmetic, 3, "00:10", "solar sky/light flash + boom",            Risk::Safe },

        // ---- Sound (audio cue) ----
        { "alienSounds", D::SpecialEvent, C::Sound, -1, "rep-gated", "noiser_C x3 (alien noises)",     Risk::Safe },

        // ---- Scare (per-player jumpscare / hallucination) ----
        { "toeStab",    D::RunEvent, C::Scare, 23, "02:03",   "per-player toe-stab scare",             Risk::Caution },
        { "falseEnter", D::RunEvent, C::Scare, -1, "trigger", "per-player false-enter scare",          Risk::Caution },
        { "mann",       D::RunEvent, C::Scare, -1, "trigger", "per-player mannequin scare",            Risk::Caution },
        { "vent",       D::RunEvent, C::Scare, -1, "trigger", "per-player vent scare",                 Risk::Caution },
        { "fakeGrays",  D::RunEvent, C::Scare, -1, "trigger", "per-player gray hallucination",         Risk::Caution },

        // ---- Teleport (relocates the triggering player) ----
        { "earthTp", D::RunEvent, C::Teleport, 26, "11:27", "teleports the triggering player",          Risk::Dangerous },

        // ---- Physics ----
        { "agrav", D::RunEvent, C::Physics, 16, "01:00", "anti-gravity (gated isPhysicalEvents)",       Risk::Caution },

        // ---- Dream (sleep subsystem) ----
        { "bedEvent",       D::RunEvent,     C::Dream, -1, "trigger", "sleep/dream (trigger_bedEvent)",  Risk::Caution },
        { "treehouseSleep", D::SpecialEvent, C::Dream, -1, "rep-gated","treehouse kidnapping (sleep -> teleport)", Risk::Caution },

        // ---- World (non-prop device / world state) ----
        { "vehtp",      D::RunEvent,     C::World, -1, "trigger",  "ATV teleport",                       Risk::Caution },
        { "crys",       D::RunEvent,     C::World, -1, "trigger",  "event_crys",                         Risk::Safe },
        { "console",    D::SpecialEvent, C::World, -1, "rep-gated","terminal types commands (event_consoleWrite_C)", Risk::Caution },
        { "lightswitch",D::SpecialEvent, C::World, -1, "rep-gated","lights turn off (event_lightsTurnoffer_C)", Risk::Caution },
        { "keypadGuess",D::SpecialEvent, C::World, -1, "rep-gated","door keypad guessed (event_passwordGuesser_C)", Risk::Caution },
        { "atvExplode", D::SpecialEvent, C::World, -1, "rep-gated","ATV boobytrap (car.trap = true)",    Risk::Dangerous },
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
    // Copy the POD fields -- the GT task may outlive this render-thread frame.
    const std::string name = ev.name;
    const Dispatch dispatch = ev.dispatch;
    GT::Post([name, dispatch] {
        void* eventer = Eventer();
        if (!eventer) {
            UE_LOGW("event_trigger: no live trigger_eventer (world not up?)");
            return;
        }
        const std::wstring wname(name.begin(), name.end());
        if (dispatch == Dispatch::SpecialEvent) {
            // A specific, addressable ariral prank: runSpecialEvent(name) -- a flat per-name switch,
            // NO rep/random gating (the RANDOM path is RandomPrank below). 2 of the 36 cases are
            // no-ops (agrav -> use runEvent; arirEgg -> April-Fools-gated); those are not exposed.
            if (!g_runSpecialEventFn) {
                UE_LOGW("event_trigger: runSpecialEvent unresolved -- cannot fire prank '%s'", name.c_str());
                return;
            }
            ue_wrap::ParamFrame f(g_runSpecialEventFn);
            if (!f.valid()) return;
            f.Set<R::FName>(L"eventName1", ue_wrap::fname_utils::StringToFName(wname));
            if (ue_wrap::Call(eventer, f))
                UE_LOGI("event_trigger: runSpecialEvent('%s') dispatched", name.c_str());
            else
                UE_LOGW("event_trigger: runSpecialEvent('%s') dispatch FAILED", name.c_str());
            return;
        }
        // RunEvent + RandomPrank both go through runEvent(Event, Special). RandomPrank keys on
        // Special="ariralPrank" (Event ignored -> summonArirPrank random rep-tier draw); a normal
        // event passes its own name with Special="None".
        ue_wrap::ParamFrame f(g_runEventFn);
        if (!f.valid()) return;
        const bool random = (dispatch == Dispatch::RandomPrank);
        const std::wstring eventName   = random ? L"arirInteraction_0" : wname;
        const std::wstring specialName = random ? L"ariralPrank" : L"None";
        f.Set<R::FName>(L"event", ue_wrap::fname_utils::StringToFName(eventName));
        f.Set<R::FName>(L"special", ue_wrap::fname_utils::StringToFName(specialName));
        if (ue_wrap::Call(eventer, f))
            UE_LOGI("event_trigger: runEvent('%ls'%s) dispatched", eventName.c_str(),
                    random ? ", special=ariralPrank" : "");
        else
            UE_LOGW("event_trigger: runEvent('%ls') dispatch FAILED", eventName.c_str());
    });
    return true;
}

}  // namespace coop::dev::event_trigger
