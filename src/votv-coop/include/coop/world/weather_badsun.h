// coop/world/weather_badsun.h -- the Bad Sun, its existence owned by the host.
//
// badSun_C is a world-event actor every peer expresses itself: a copy polls the shared clock and, at the first
// poll inside 07:00..18:00, shows its widget and damages its own player (ue_wrap/world/bad_sun), so the sky and the
// harm are peer-local by the game's own design. What the host owns is WHETHER a copy exists. A fresh one comes from
// the gamemode's `Spawn Bad Sun` (the day cycle's midnight roll, which no ProcessEvent hook sees, and the cheat
// menu) and the end from the copy's own remove(); the script gate sees both on every route. A CLIENT refuses its own
// `Spawn Bad Sun` at the gate and applies the host's through the same verb, or destroys an inactive copy; an active
// copy ends by its own clock. The red sky's lane is the template (coop/world/weather_redsky).

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct BadSunPayload; }

// MTA's shape, which this follows. The HOST sends its state at the call that changes it, as MTA's server stores a
// weather change and broadcasts it at SetWeather
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:11118-11134) to joined players
// only (reference/mtasa-blue/Server/mods/deathmatch/logic/CPlayerManager.cpp:159-172); a joiner is seeded at its
// world-ready (coop/world/weather_sync), as MTA's join packet carries the current weather
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CMapManager.cpp:537). Where it leaves it: the seed and the
// edge carry existence and no phase, so a joiner's widget ramp restarts, up to 60 s of grace, accepted because the
// effect is peer-local (MTA's join packet carries the blend phase,
// reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:2249-2260).

namespace coop::weather_badsun {

// Cache the session, before weather_sync's install latch: a client's payload that arrives while that latch still
// waits is kept, not dropped as sessionless. Called first in weather_sync::Install, as weather_lightning::SetSession is.
void SetSession(coop::net::Session* session);

// Once, watch `Spawn Bad Sun` and badSun_C's `remove` at the script gate. Called from weather_sync::Install on every
// re-entry, after SetSession has cached the session.
void Install();

// Say, once each, whether each watch is live (final once its class has loaded), and apply the host's Bad Sun that
// Apply kept for a world with no gamemode or day cycle, once this world has both. Called from
// weather_sync::TickConnect; a few bool reads once both watches are said and nothing waits.
void Tick();

// Read whether the running world's gamemode holds a live Bad Sun: the seed a joiner's world-ready gets (principle
// 8). Returns whether it could read: false, with `present` false, off the game thread or when `gamemode.badSun`
// does not read.
bool ReadLocalBadSun(bool& present);

// The host's state on this client: spawn one through the gamemode's own verb when it has none, destroy its inactive
// copies when the host has none. A world with no gamemode or no day cycle keeps the payload, the newest, and Tick
// applies it once the world has both; a payload that applies, or that a newer one reaches, leaves no older one kept.
// A client's call: with no session, or on the host, it returns at once. Game thread.
void Apply(const coop::net::BadSunPayload& payload);

// This session's counts and the last apply that counted the copies, for a drill to judge a run by (the lane's own
// state, not its log). The totals are the ones OnDisconnect says. `removesRan`: copies of this peer whose own remove()
// ran. `sentAbsent`: the host's sends of "no Bad Sun". `lastApply*`: the newest apply that proceeded to count the live
// copies (`lastApplied` unset until one did): the state it was asked for, the copies it counted, and whether it
// spawned one. Game thread.
struct Counts {
    uint64_t refused = 0, sent = 0, sentAbsent = 0, spawned = 0, destroyed = 0, leftActive = 0, applyFailed = 0;
    uint64_t removesRan = 0;
    bool lastApplied = false, lastApplyWant = false, lastApplySpawned = false;
    int lastApplyLive = 0;
};
Counts GetCounts();

// The session ended: its counts are said and cleared, and a kept payload dropped.
void OnDisconnect();

}  // namespace coop::weather_badsun
