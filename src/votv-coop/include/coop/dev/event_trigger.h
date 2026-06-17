// coop/dev/event_trigger.h -- DEV: trigger any game event from the F1 menu
// (user directive 2026-06-13: the FULL event category, host-only, for sync/
// mirror testing).
//
// RE (votv-event-system-RE-2026-06-13.md): the entire event system funnels
// through ONE verb -- gamemode.eventer.runEvent(FName event, FName special)
// on the single placed trigger_eventer_C -- the exact path the game's own
// ui_eventRun debug widget uses. 65 SwitchName cases + the ariralPrank
// special branch = 66 entries; an unknown name is a safe no-op (switch
// default). No stop verb exists: events self-terminate (lib.setEvent
// refcount). Day numbers shown are the DataTable unlock day (time.Z) --
// informational only, runEvent itself does not gate on them.
//
// HOST-ONLY by design: clients never call runEvent (dev_gate refuses, same
// as every dev verb); the spawned actors/NPCs mirror through the existing
// pipelines -- which is precisely what these triggers exist to test.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::dev::event_trigger {

enum class Risk : uint8_t {
    Safe = 0,       // cosmetic/prop/self-terminating -- good first sync tests
    Caution = 1,    // spawns hostiles / touches a synced subsystem
    Dangerous = 2,  // story/save progression or player relocation -- Ctrl+click
};

struct EventInfo {
    const char* name;      // the runEvent FName (ASCII)
    int dayZ;              // DataTable unlock day; -1 = story/trigger-only row
    const char* category;  // the RE doc's grouping (display)
    Risk risk;
    bool prank;            // true = the ariralPrank special branch
    const char* trigger;   // how the GAME natively fires it (NOT our F1 runEvent) -- shown in the menu.
                           // "random daily roll (day>=N)" = eligible to be randomly selected from day N
                           // (the nightly selector is native -- the DataTable is the proven day-gate, the
                           // picker itself is C++/level-trigger, not in the BP dumps). "day N story" =
                           // scripted on reaching the day. Sources: list_events DataTable +
                           // votv-event-system-RE-2026-06-13.md + the 2026-06-17 classification.
};

// The full cooked table (static -- the SwitchName is fixed per game build).
const std::vector<EventInfo>& Events();

// Reflected runEvent(event, special) on the host. Refuses on a connected
// client (dev_gate) and while the eventer/verb is unresolved. Game thread
// (call from the menu's render path is fine -- it posts to the game thread).
bool Trigger(const EventInfo& ev);

}  // namespace coop::dev::event_trigger
