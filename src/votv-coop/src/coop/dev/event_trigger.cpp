// coop/dev/event_trigger.cpp -- see coop/dev/event_trigger.h.

#include "coop/dev/event_trigger.h"

#include "coop/dev/dev_gate.h"
#include "coop/world/event_fire_sync.h"
#include "ue_wrap/call.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <string>

// The eventer resolution + the reflected runEvent/runSpecialEvent dispatch moved to
// coop/world/event_fire_sync (v95, one owner): the menu's fire must be the SAME seam that
// broadcasts EventFire to clients (a direct runEvent never reaches passEvents, so the host
// observation poll cannot see menu fires). Only the ambient/weather verb table stays here --
// those are dev-only levers on daynightCycle/mainGamemode timers, never on the wire.

namespace coop::dev::event_trigger {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

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
        case Category::Weather:  return "Weather";
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
        // arirGraff: the switch has ONLY the 7 per-variant cases (arirGraff_0..6) -- a bare
        // "arirGraff" falls through the SwitchName to RETURN, a silent no-op (2026-07-03 fix
        // of the exact bug the user reported: "не все ивенты в списке").
        { "arirGraff_0",  D::SpecialEvent, C::Prop, -1, "rep-gated","graffiti decal variant 0 (grime_arirGraffiti_C)", Risk::Safe },
        { "arirGraff_1",  D::SpecialEvent, C::Prop, -1, "rep-gated","graffiti decal variant 1", Risk::Safe },
        { "arirGraff_2",  D::SpecialEvent, C::Prop, -1, "rep-gated","graffiti decal variant 2", Risk::Safe },
        { "arirGraff_3",  D::SpecialEvent, C::Prop, -1, "rep-gated","graffiti decal variant 3", Risk::Safe },
        { "arirGraff_4",  D::SpecialEvent, C::Prop, -1, "rep-gated","graffiti decal variant 4", Risk::Safe },
        { "arirGraff_5",  D::SpecialEvent, C::Prop, -1, "rep-gated","graffiti decal variant 5", Risk::Safe },
        { "arirGraff_6",  D::SpecialEvent, C::Prop, -1, "rep-gated","graffiti decal variant 6", Risk::Safe },
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

        // ---- Weather (the AMBIENT layer -- daynightCycle/mainGamemode timer verbs, not the eventer;
        //      each button calls the SAME UFunction the game's own timer/RNG calls, on the live instance) ----
        { "spawnFog",       D::Ambient, C::Weather, -1, "ambient", "thick rolling fog NOW (weatherFogController_C, 5-20 min)", Risk::Safe },
        { "rain ON",        D::Ambient, C::Weather, -1, "ambient", "daynightCycle.causeRain(true)",                Risk::Safe },
        { "rain OFF",       D::Ambient, C::Weather, -1, "ambient", "daynightCycle.causeRain(false)",               Risk::Safe },
        { "spawnRedSky",    D::Ambient, C::Weather, -1, "ambient", "red-sky ambience (mainGamemode.spawnRedSky)",  Risk::Caution },
        { "spawnBlackFog",  D::Ambient, C::Weather, -1, "ambient", "BLACK fog + eyer_C ghosts (mainGamemode)",     Risk::Caution },
        { "badSun",         D::Ambient, C::Weather, -1, "ambient", "bad sun (mainGamemode.'Spawn Bad Sun')",       Risk::Caution },
        { "flowerSpawner",  D::Ambient, C::Weather, -1, "ambient", "anomalous flowers (mainGamemode.flowerSpawner)", Risk::Safe },
        { "trySpawnInsomniac", D::Ambient, C::Weather, -1, "ambient", "insomniac spawn attempt (internally gated -- may no-op)", Risk::Caution },
    };
    return kEvents;
}

bool Trigger(const EventInfo& ev) {
    if (!::coop::dev_gate::Allowed()) {
        UE_LOGW("event_trigger: REFUSED -- dev features are disabled while connected as a client");
        return false;
    }
    // Copy the POD fields -- the GT task may outlive this render-thread frame.
    const std::string name = ev.name;
    const Dispatch dispatch = ev.dispatch;
    if (dispatch != Dispatch::Ambient) {
        // The three eventer paths route through the SHARED fire seam (native dispatch +
        // the v95 EventFire broadcast so connected clients replay per policy).
        namespace efs = coop::event_fire_sync;
        const std::wstring wname(name.begin(), name.end());
        const bool random = (dispatch == Dispatch::RandomPrank);
        const bool special = (dispatch == Dispatch::SpecialEvent);
        // false only on the client-role refusal (HostFire logs it); resolution + the world-up
        // check happen inside the posted game-thread task and warn loudly on their own.
        return efs::HostFire(
            special ? efs::FireKind::SpecialEvent : efs::FireKind::RunEvent,
            random ? L"arirInteraction_0" : wname,
            random ? L"ariralPrank" : L"None");
    }
    // Ambient/weather layer: these verbs are NOT eventer cases -- the game fires them from
    // daynightCycle/mainGamemode timers + RNG rolls. "Maximally native" here = call the SAME
    // UFunction the timer's success-arm calls, on the live instance (the exact pattern the
    // autonomous fogprobe proved; autotest_fog_probe.cpp). Dev-only, never on the wire.
    GT::Post([name] {
        struct AmbientVerb { const wchar_t* cls; const wchar_t* fn; int boolArg; const wchar_t* boolName; };
        static const struct { const char* key; AmbientVerb v; } kAmbient[] = {
            { "spawnFog",          { L"daynightCycle_C", L"spawnFog",          -1, nullptr } },
            { "rain ON",           { L"daynightCycle_C", L"causeRain",          1, L"isRaining" } },
            { "rain OFF",          { L"daynightCycle_C", L"causeRain",          0, L"isRaining" } },
            { "spawnRedSky",       { L"mainGamemode_C",  L"spawnRedSky",       -1, nullptr } },
            { "spawnBlackFog",     { L"mainGamemode_C",  L"spawnBlackFog",     -1, nullptr } },
            { "badSun",            { L"mainGamemode_C",  L"Spawn Bad Sun",     -1, nullptr } },
            { "flowerSpawner",     { L"mainGamemode_C",  L"flowerSpawner",     -1, nullptr } },
            { "trySpawnInsomniac", { L"mainGamemode_C",  L"trySpawnInsomniac", -1, nullptr } },
        };
        const AmbientVerb* verb = nullptr;
        for (const auto& a : kAmbient) if (name == a.key) { verb = &a.v; break; }
        if (!verb) { UE_LOGW("event_trigger: unknown ambient verb '%s'", name.c_str()); return; }
        void* inst = nullptr;
        for (void* obj : R::FindObjectsByClass(verb->cls)) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) {
                inst = obj;
                break;
            }
        }
        if (!inst) { UE_LOGW("event_trigger: no live %ls instance (world not up?)", verb->cls); return; }
        void* fn = R::FindFunction(R::ClassOf(inst), verb->fn);
        if (!fn) { UE_LOGW("event_trigger: %ls::%ls unresolved", verb->cls, verb->fn); return; }
        ue_wrap::ParamFrame f(fn);
        if (!f.valid()) return;
        if (verb->boolArg >= 0) f.Set<bool>(verb->boolName, verb->boolArg != 0);
        if (ue_wrap::Call(inst, f))
            UE_LOGI("event_trigger: ambient %ls::%ls dispatched ('%s')", verb->cls, verb->fn, name.c_str());
        else
            UE_LOGW("event_trigger: ambient %ls::%ls dispatch FAILED", verb->cls, verb->fn);
    });
    return true;
}

}  // namespace coop::dev::event_trigger
