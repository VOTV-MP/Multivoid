// coop/dev/event_trigger.h -- DEV: trigger any game event from the F1 menu
// (user directive 2026-06-13 + 2026-06-17: the FULL event list, strict
// categories, host-only, with each event's NATIVE trigger time-of-day).
//
// RE (votv-event-system-RE-2026-06-13.md + the 2026-06-17 dispatcher sweep):
// the event system funnels through TWO FName dispatchers on the single placed
// trigger_eventer_C:
//   - runEvent(FName event, FName special)  -- 65 SwitchName cases (the bulk).
//   - runSpecialEvent(FName eventName1)->bool -- 36 cases (the ariral PRANKS),
//     a FLAT per-name switch with NO rep/random gating, so each prank is
//     INDIVIDUALLY addressable by name (2 of the 36 are no-ops -> 34 exposed).
// The game's OWN normal prank path is runEvent(_, "ariralPrank") -> summonArirPrank,
// which RANDOMIZES over a reputation-tier pool (the incoming name is discarded);
// we keep one "random ariral prank" entry for that, PLUS the 24 prank-only names
// via runSpecialEvent for deterministic per-prank testing.
//
// NATIVE TRIGGER TIME: the daynightCycle clock decomposes as X=Hour, Y=Minute,
// Z=Day (daynightCycle::getNamedTime, byte-verified). Each list_events row's
// time.X:time.Y is the event's authored daily anchor (e.g. morningGay 10:50,
// arirSignal 18:00). It is the correct "native trigger time" LABEL; the nightly
// roller that compares the clock to it is native/undumped, so treat HH:MM as the
// scheduled anchor, not a frame-exact fire moment. Story builds are trigger-driven
// (no clock anchor); pranks are reputation-gated (no clock schedule).
//
// HOST-ONLY by design: clients never dispatch (dev_gate refuses, same as every
// dev verb); the spawned actors/NPCs mirror through the existing coop pipelines --
// which is precisely what these triggers exist to test.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::dev::event_trigger {

enum class Risk : uint8_t {
    Safe = 0,       // cosmetic/prop/self-terminating -- good first sync tests
    Caution = 1,    // spawns hostiles / touches a synced subsystem
    Dangerous = 2,  // story/save progression or player relocation -- Ctrl+click
};

// STRICT, CLOSED category set (the 2026-06-17 classification + dispatcher RE).
// Every event maps to exactly one. The menu groups by this.
enum class Category : uint8_t {
    Story,     // narrative/save progression (treehouse, server breaks, obelisk/pyramid, looker arc, ending)
    Ariral,    // ariral-faction NPCs / reputation interactions + the addressable pranks
    Creature,  // spawns a hostile/ambient creature or invasion swarm
    Signal,    // a SETI / forceObjects "signal" beat
    Prop,      // spawns/removes world prop(s); no creature or story progression
    Cosmetic,  // pure sky/particle/visual
    Sound,     // primary effect is an audio cue
    Scare,     // per-player jumpscare / hallucination by design
    Teleport,  // relocates the triggering player
    Physics,   // loose-physics manipulation
    Dream,     // enters the sleep/dream subsystem
    World,     // mutates non-prop world/device state (ATV, keypad/console/lights devices)
    Weather,   // the AMBIENT layer: fog/rain/sky verbs fired by daynightCycle/mainGamemode timers+RNG,
               // NOT by the eventer dispatchers -- the dev trigger calls the SAME UFunction the game's
               // own timer calls on the live instance (the fogprobe-proven pattern). 2026-07-03.
    COUNT
};

// Display name for a category (the menu's section header). Stable, ASCII.
const char* CategoryName(Category c);

// How the menu dispatches the event.
enum class Dispatch : uint8_t {
    RunEvent,      // runEvent(name, "None") -- the normal 65-case path
    RandomPrank,   // runEvent("arirInteraction_0", "ariralPrank") -- a RANDOM rep-tiered ariral prank
    SpecialEvent,  // runSpecialEvent(name) -- ONE specific, addressable ariral prank
    Ambient,       // an ambient/weather verb on daynightCycle_C / mainGamemode_C (ev.name keys the
                   // verb table in the .cpp) -- the exact UFunction the game's own timer/RNG calls.
                   // NOT exposed (documented, deliberate): superFogEvent (a literal 5%% roll inside --
                   // a deterministic lever needs the superFog_C spawn transform, follow-up),
                   // fleshRain/spawnErrorObject (need a spawn transform), skysphere setEye +
                   // jellyfishPath spawn (verb/owner not confirmed in the current CXX dump).
};

struct EventInfo {
    const char* name;       // the FName dispatched (ASCII). For RandomPrank this is the display label only.
    Dispatch    dispatch;   // which engine path fires it
    Category    cat;        // strict category (the menu groups by this)
    int         dayZ;       // unlock day (time.Z); -1 = story/trigger-only or rep-gated (no DataTable day)
    const char* time;       // native trigger time-of-day: "HH:MM" anchor / "trigger" (build/story) / "rep-gated" (prank pool)
    const char* mechanism;  // what it natively spawns/does (one short clause)
    Risk        risk;
};

// The full table (static -- the dispatcher SwitchNames are fixed per game build).
// Ordered BY CATEGORY (then day) so the menu's grouped layout reads cleanly.
const std::vector<EventInfo>& Events();

// Reflected dispatch on the host (runEvent / runSpecialEvent per ev.dispatch).
// Refuses on a connected client (dev_gate) and while the eventer/verb is
// unresolved. Safe to call from the menu render path (posts to the game thread).
bool Trigger(const EventInfo& ev);

}  // namespace coop::dev::event_trigger
