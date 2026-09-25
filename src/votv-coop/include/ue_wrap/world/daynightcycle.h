// ue_wrap/world/daynightcycle.h -- standalone engine access for VOTV's world clock
// (daynightCycle_C, the singleton owning time of day and the weather scheduler). A principle-7
// engine wrapper over the cycle's CLOCK fields: no network logic, no coop state, both of which
// coop/world/time_sync owns and drives through here.
//
// The clock is three floats. `day` is the WITHIN-day accumulator, advanced each tick by
// deltaSeconds * timeScale and then by the difficulty, day-speed and sleep multipliers, and
// wrapped modulo `maxTime` at the roll; `totalTime` takes the same increment and is never
// wrapped, so it is the absolute elapsed clock; `timeScale` is the rate. Sun and moon follow
// `day`, not totalTime: the cycle recomputes phase = ((day + offset) % maxTime) / maxTime and
// rotates the lights by phase * 360 on its own sun-rotation timer, not every tick, so writing
// `day` makes the sun follow within that period and we never drive the light fields ourselves.
// The native loadtime(totalTime, day) writes both members and only while `skipDaySet` is
// false -- with that flag set it writes nothing at all -- and fans out to nothing else.

#pragma once

#include <cstdint>

namespace ue_wrap::daynightcycle {

// Resolve the daynightCycle_C UClass + the totalTime / Day / TimeScale field offsets.
// Idempotent; true once resolved (false while the BP class is not yet loaded -- the
// caller retries on a later tick). Game thread.
bool EnsureResolved();

// The live AdaynightCycle_C singleton (cached; re-resolved if it dies). nullptr until it
// has streamed in. Game thread.
void* Cycle();

// The cycle's own ReceiveTick event (declared on daynightCycle_C, dispatched by the engine through
// ProcessEvent at each actor tick -- every frame by the class's defaults, which set no tick
// interval), the seam a lane parks the cycle at: a pre-observer on it runs right before the tick
// reads the clock. Null until EnsureResolved has succeeded. Game thread.
void* TickFunction();

// The save slot of the gamemode a given cycle belongs to, by its own `gamemode` member: no walk, so
// a tick observer may call it every frame. Null while anything on the way is unresolved or dead.
// Game thread.
void* SaveSlotOfCycle(void* cycle);

// Whether a cycle is the main menu's: its gamemode's own isMainMenu, which the gamemode sets when
// its level is the menu scene, which ticks a cycle of its own. By the cycle's `gamemode` member, so
// a tick observer may call it every frame. False while unresolved. Game thread.
bool IsMenuCycle(void* cycle);

// Read the cycle's clock into the outs. False if the cycle / offsets are not resolved
// (outs untouched on failure). Game thread.
bool ReadClock(float& totalTime, float& day, float& timeScale);
// The same on a given cycle, for a caller the engine has just handed one (a tick observer's own
// `self`): no cached singleton, so no world-memo lag and no walk. False if `cycle` is null.
bool ReadClockOf(void* cycle, float& totalTime, float& day, float& timeScale);

// Read maxTime -- the length of ONE day in `day` units; the within-day clock runs [0, maxTime)
// and wraps at the midnight roll. Lets a caller map a time-of-day FRACTION (0..1) onto a `day`
// value, which is how the named clock reads it. The SUN is that fraction plus a mode-dependent
// offset, so it is not the same angle. False if unresolved. Game thread.
bool ReadMaxTime(float& maxTime);
bool ReadMaxTimeOf(void* cycle, float& maxTime);  // on a given cycle, as ReadClockOf

// Overwrite the cycle's two accumulators by direct field write, totalTime and day, as loadtime
// writes them but without its skipDaySet test. The cycle re-derives the sun from `day`, and its
// next tick rebuilds the named clock from it. No-op if unresolved. Game thread.
void ApplyClock(float totalTime, float day);
void ApplyClockOf(void* cycle, float totalTime, float day);  // on a given cycle, as ReadClockOf

// ---- the NAMED clock: `timeZ` (FIntVector: X=hour, Y=minute, Z=day number) ----
// The game's own running triple, and derived: the cycle rebuilds it every tick -- hour and minute
// from `day` and `maxTime`, Z copied from saveSlot.savedtime.Z, which is where the DAY NUMBER
// lives -- and hands it to saveSlot.settime, which raises the new-minute and new-hour pulses on
// any difference from savedtime. A write here lasts until that tick, so there is no writer. Its
// new-day flag is read nowhere; the day roll is the tick threshold `day > maxTime` instead,
// which is also what increments savedtime.Z. The float `day` is NOT the day number: it is the
// within-day accumulator that threshold fires on. Game thread; false if unresolved.
bool ReadTimeZ(int32_t& hour, int32_t& minute, int32_t& day);

// saveSlot.savedtime, the same triple as the save keeps it. Its Z is the day number's SOURCE: the
// cycle copies it into timeZ.Z every tick and the day roll increments it, and saveSlot.settime
// rewrites the whole triple whenever the hour or the minute moves. False until the gamemode and
// its saveSlot resolve. Game thread.
bool ReadSavedTime(int32_t& hour, int32_t& minute, int32_t& day);
// The same in a given save slot (SaveSlotOfCycle's): no gamemode lookup. False if `saveSlot` is null.
bool ReadSavedTimeOf(void* saveSlot, int32_t& hour, int32_t& minute, int32_t& day);

// Write the day number, savedtime.Z, alone: the hour and minute stay settime's, so the next tick
// sees them move and raises its pulses as it would for any clock change. A raw write, as the day
// roll's own increment is. False until the saveSlot resolves. Game thread.
bool WriteSavedDay(int32_t day);
bool WriteSavedDayOf(void* saveSlot, int32_t day);  // in a given save slot, as ReadSavedTimeOf

// The cycle's rate inputs, read-only, for instruments. Outside realtime mode each tick adds
// deltaSeconds * timeScale * diffMult * settingMultiplayer * sleepingTimeDilation to `day`; in
// realtime mode `day` is the machine's own wall-clock hour and the day rolls at its midnight.
// False if the cycle or a field is unresolved. Game thread.
struct Rates {
    bool  realtime = false;
    float diffMult = 0.f;
    float settingMultiplayer = 0.f;
    float sleepingTimeDilation = 0.f;
};
bool ReadRates(Rates& out);

// ---- the client's parked clock (coop/world/time_sync drives these) ----
// The midnight cascade -- the hash codes, the task roll, the Bad Sun, the results mail and points
// -- is a tick threshold, `day > maxTime` inside the cycle's own tick chain, armed on every peer.
// A client's cycle is held at timeScale 0, so its own advance never moves `day`, and the last host
// sample is written in before each ReceiveTick, over a local write; a sample's `day` is below maxTime,
// as the host wraps inside its own tick, so the cascade is out of its reach while the sky keeps
// deriving from `day`. The gamemode's begin-play call enters the tick body once per world without
// that write, on the loaded `day`, below maxTime too. The hour pulse still runs, and func_newHour
// would place the automatic 6 am drone order while dailyDelivery is false, so that is latched too.

// Write timeScale alone: 0 to park a client's clock, 1.0f -- the game's own running value, the
// one its rewind restores after the hour it spends at -1 -- to hand it back. No-op if unresolved.
void WriteTimeScale(float scale);
void WriteTimeScaleOf(void* cycle, float scale);  // on a given cycle, as ReadClockOf

// saveSlot.dailyDelivery := true in a given save slot -- the game's OWN 6am-order latch (its only
// writers are the suppressed midnight reset and the order placement itself, so one write holds for
// the session). Called with every clock correction; cheap (a cached offset and one bool write).
// False if `saveSlot` is null or the field is unresolved.
bool LatchDailyDeliveryOf(void* saveSlot);

// ---- the rollover's per-machine outputs (coop/world/day_edge performs them on a client) ----
// A cycle's sleeplessDays: the midnights since its world loaded, the cycle's own member and never saved.
// Every rollover adds one, asleep or awake, and the `insomniac` achievement progresses at seven. False
// if the cycle is null or the member does not resolve.
bool ReadSleeplessDaysOf(void* cycle, int32_t& out);
bool WriteSleeplessDaysOf(void* cycle, int32_t v);

// A save slot's musics[]: one flag a day's music sting, cleared by the sting's check as its time comes,
// whether or not the sting plays. The rollover sets them all again. How many are set, of how many; and every one written `set`. False if the slot is null or the
// member does not resolve.
bool ReadMusicsOf(void* saveSlot, int32_t& set, int32_t& count);
bool WriteAllMusicsOf(void* saveSlot, bool set);

}  // namespace ue_wrap::daynightcycle
