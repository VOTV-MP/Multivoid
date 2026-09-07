// coop/props/registry_reaper.h -- the prop-registry <-> world-lifecycle
// reconciler (extracted from coop/session/net_pump.cpp 2026-07-18; net_pump
// sat at 1237 LOC with a 915-line Tick).
//
// ONE concept: keep the Prop Element registry coherent with the world's actor
// population across every world-lifecycle event. Owns (bodies verbatim from
// net_pump's old reaper block): the throttled dead-element reaper, purge-
// EPISODE detection (mass GC purge = level/world transition) + the episode-end
// world-change re-seed (+ post-purge key-index drain + save-identity re-bind),
// the small-travel re-seed companion, the steady-world high-water re-seed
// (spawn-menu/toolgun/ambient adoption + incremental PropSpawn delivery), the
// host death-watch PropDestroy broadcast, and the gameplay->menu TRANSITION
// DETECTION (the RAM-balloon guard's world scan lives here because the world
// scan is this module's core competency; the flee ACTION stays session-owned
// -- net_pump::FleeAfterNativeMenuTravel). The announce axis likewise keeps
// ONE owner: this module only calls net_pump::MaybeRequestReAnnounce.
// Game thread only (called from net_pump::Tick).

#pragma once

namespace coop::net { class Session; }

namespace coop::registry_reaper {

// The ~4s-throttled reaper and re-seed scan, extracted whole from
// net_pump. Returns true iff the gameplay->menu guard fired (session torn
// down + fleeing) -- the caller must abort its Tick, exactly as the old
// inline `return` did. Game thread.
bool Tick(coop::net::Session& session);

// Session-start reset for the module's cross-session state (the
// been-in-gameplay latch the menu guard reads). Called from
// net_pump::OnSessionStart.
void OnSessionStart();

}  // namespace coop::registry_reaper
