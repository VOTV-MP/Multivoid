// coop/time_sync.h -- host-authoritative WORLD CLOCK sync (time-of-day) (protocol v36).
//
// Gameplay/network layer (principle 7): owns the wire protocol + the host poll + the
// client apply + the connect-snapshot. Talks to the engine ONLY through
// ue_wrap::daynightcycle.
//
// WHY: the cycle's clock (totalTime/Day/TimeScale) is NOT otherwise replicated, so a
// fresh joiner free-runs its own day-0/night clock while the host is at midday -> the
// client world renders DARK. The sun is a pure function of totalTime (the cycle's
// ReceiveTick re-derives it each frame), so syncing the clock fixes the brightness.
//
// MODEL (host-authoritative, MTA single-syncer): the HOST polls its cycle and broadcasts
// the clock on a throttle (the clock is continuous -> push periodically, not on-change) +
// on a joiner's connect edge. The CLIENT direct-writes the three floats; writing TimeScale
// lets the client free-run between pushes so the sun doesn't step. The client never drives
// the sun/light fields (the purple-light lesson) -- only the clock.
//
// Distinct from weather_sync (rain/fog/lightning/redsky) on purpose: the clock is its own
// subsystem (modular rule -- weather_sync is already over the soft cap). The migration
// roadmap's coop::WorldState would later consolidate both; this is the focused clock fix.
//
// RE: research/findings/votv-coop-class-clone-migration-roadmap-2026-06-06.md §2.

#pragma once

namespace coop::net {
class Session;
struct TimeSyncPayload;
}  // namespace coop::net

namespace coop::time_sync {

// Store the session pointer + resolve the cycle (idempotent; retried each tick until the
// daynightCycle_C BP class loads). Game thread.
void Install(coop::net::Session* session);

// CLIENT receiver: a TimeSync packet arrived -- apply the host's authoritative clock to the
// local cycle. No-op on the host. Called from event_feed's reliable drain. Game thread.
void OnReliable(const coop::net::TimeSyncPayload& payload);

// HOST: send the current clock to a freshly connected client `peerSlot` immediately (so the
// joiner's world isn't dark until the first throttled push). Net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick pump: HOST throttled poll + broadcast. No-op on the client / when solo. Call every
// net-pump tick on the game thread.
void Tick();

// v71 sleep gate (coop/sleep_sync): while the accelerate phase runs, the CLIENT clock
// free-runs at TimeScale=1 (its world is dilated 20x, matching the host's advance rate)
// instead of the U6 TimeScale=0 -- otherwise the timelapse sky only moves on the 2 s
// corrections and pans in visible steps. Toggled at the phase edges; applies immediately
// and on every subsequent correction. time_sync stays the ONLY TimeScale writer (one
// authority). No-op on the host. Game thread.
void SetSleepAccelerate(bool on);

// Session teardown: reset the throttle. Game thread.
void OnDisconnect();

}  // namespace coop::time_sync
