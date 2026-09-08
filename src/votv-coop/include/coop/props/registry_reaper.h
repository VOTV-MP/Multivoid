// coop/props/registry_reaper.h -- the prop-registry <-> world-lifecycle reconciler: it keeps
// the Prop Element registry coherent with the world's actor population across every
// world-lifecycle event.
//
// Owns the throttled dead-element reaper; purge-EPISODE detection (a mass GC purge is a
// level/world transition) with the episode-end world-change re-seed, its post-purge key-index
// drain and its save-identity re-bind; the small-travel re-seed companion; the host's
// death-watch PropDestroy broadcast; and the gameplay->menu TRANSITION DETECTION. The steady
// re-seed is NOT here: prop_census.cpp runs it as a scan-hub consumer with a budgeted drain.
//
// Two axes deliberately keep an owner elsewhere. The RAM-balloon guard's world scan lives here,
// the world scan being this module's core competency, while the flee ACTION stays session-owned
// (net_pump::FleeAfterNativeMenuTravel); and the announce axis is reached only through
// net_pump::MaybeRequestReAnnounce. Game thread only (called from net_pump::Tick).

#pragma once

namespace coop::net { class Session; }

namespace coop::registry_reaper {

// The ~4 s-throttled reaper and re-seed scan. Returns true iff the gameplay->menu guard fired
// (the session torn down and fleeing), in which case the caller must abort its own Tick. Game
// thread.
bool Tick(coop::net::Session& session);

// Session-start reset for the module's cross-session state: the been-in-gameplay latch the menu
// guard reads. Called from net_pump::OnSessionStart.
void OnSessionStart();

}  // namespace coop::registry_reaper
