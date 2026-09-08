// coop/items/hook_sync.h -- Grappling hook and rope visual synchronization.
// Handles synchronization of Ahook_C (hook_C, rope_C, etc.) across peers in all states:
//   1. Thrown / flying (hook flying, cable extending from player hand).
//   2. Held / hooked to player (cable anchored to remote player puppet hand).
//   3. Attached to surfaces / props (both heads anchored, cable stretched between them).

#pragma once

#include "coop/net/protocol.h"
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::hook_sync {

// Initialize hook synchronization, installing hooks for actor spawning and destruction.
void Install(coop::net::Session* session);

// Per-frame tick: polls local hooks, broadcasts updates, and updates mirror actors.
void Tick();

// Dispatches an incoming HookSync reliable packet from the wire.
void OnHookSync(const coop::net::HookSyncPayload& payload, uint8_t senderSlot);

// HOST: Replays all active hooks to a newly joined client slot.
void QueueConnectReplayForSlot(int slot);

// Reset all local and mirror state on session disconnect.
void OnDisconnect();

}  // namespace coop::hook_sync
