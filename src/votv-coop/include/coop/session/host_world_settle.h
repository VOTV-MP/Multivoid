// coop/session/host_world_settle.h -- the host's own world has finished starting up.
//
// The host's session starts in a level that may still be coming up: the game's prop processor
// computes every prop's volume, then broadcasts finishProps to the world's actors, and the containers
// wait on that broadcast before their inventories initialize and roll their loot. Anything the host
// does in that window (a join's capture, a birth) describes a world that is not
// there yet. This module is the one answer to "has it settled", per world, latched.
//
// Settled means the processor's three phases have finished AND every container the gamemode lists has
// initialized its inventory. A walk checks that at most once a second until it passes; a world that
// never passes is admitted after a 25 s ceiling with a WARN, so a stuck world cannot hold a join
// forever. A world with nothing to start (the menu, or a game build whose world cannot be named) is
// settled at once. Host only; a client's Tick is a no-op and its Settled() is false. Game thread.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::host_world_settle {

// The most containers one walk reads. A gamemode list that fills it is treated as unresolved.
inline constexpr size_t kContainerCap = 4096;

// The ceiling, in milliseconds from the world clock's start, after which a world that has not passed
// the walk is admitted anyway.
inline constexpr uint32_t kCeilingMs = 25000;

// Milliseconds since this world's clock started (the first Tick that saw its gamemode); 0 while the
// clock has not started. Rises monotonically within a world.
uint32_t MsSinceWorldBegan();

// True when the latch belongs to the world the game is in now. Never a walk. With the red row on,
// true as soon as a gamemode exists: the old test, for the hold and the birth gate that read this.
bool Settled();

// The real test, run now and never overridden by the red row: the processor finished, every listed
// container initialized, and the list and the processor both resolved. For the drill.
bool TrueSettledNow();

// Every host pump tick: a new world resets the record, the walk runs until the latch, and the
// session's `rig: READY hosting` line is said once Settled() holds. A no-op on a client.
void Tick(coop::net::Session& session);

// The session ended: the ready line is owed again to the next session. The per-world latch stays,
// since the world did not change.
void OnSessionEnd();

}  // namespace coop::host_world_settle
