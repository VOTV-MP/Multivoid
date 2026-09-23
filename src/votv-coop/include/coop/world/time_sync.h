// coop/world/time_sync.h -- host-authoritative WORLD CLOCK sync (time of day).
//
// Gameplay/network layer: owns the wire protocol, the host poll, the client apply and the connect
// snapshot. Talks to the engine ONLY through ue_wrap::daynightcycle. Distinct from weather_sync
// (rain, fog, lightning, red sky) on purpose -- the clock is its own subsystem.
//
// WHY: the cycle's clock is not otherwise replicated, so a fresh joiner free-runs its own day-zero
// night clock while the host is at midday, and the client world renders DARK. The sun is re-derived
// from the cycle's within-day accumulator, so syncing the clock fixes the brightness.
//
// MODEL (host-authoritative, single-syncer): the HOST polls its cycle and streams the clock each time
// it has moved half a game minute and at least twice a second -- it is continuous, so it is pushed
// on its progress rather than on change -- plus a reliable push on a joiner's connect edge. The CLIENT direct-writes the three floats and stays
// FROZEN at TimeScale=0 between pushes, so its `day` never wraps maxTime locally and the midnight
// cascade stays unreachable. The client never drives the sun or light fields, only the clock.

#pragma once

#include <cstdint>

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

// Per-tick pump: HOST reads the clock and hands the net thread a sample when one is due; CLIENT
// applies a new streamed sample. No-op when solo. Call every
// net-pump tick on the game thread.
void Tick();

// Sleep gate (coop/player/sleep_sync): while the accelerate phase runs, the CLIENT clock free-runs at
// TimeScale=1 -- its world is dilated 20x, which matches the host's advance rate -- instead of the
// frozen 0. Otherwise the timelapse sky only moves on the streamed corrections and pans in visible
// steps. Toggled at the phase edges, applied immediately and on every subsequent correction.
// time_sync is the only decider of the client's TimeScale, so there is one authority. No-op on the
// host. Game thread.
void SetSleepAccelerate(bool on);

// CLIENT: the host's day number as the last applied clock correction carried it (its timeZ.Z),
// -1 before the first and after a disconnect. Read-only, for instruments: the client's own day
// number is rebuilt from its save every tick and is not the host's. Any thread.
int32_t LastHostDayZ();

// Session teardown: reset the throttle. Game thread.
void OnDisconnect();

}  // namespace coop::time_sync
