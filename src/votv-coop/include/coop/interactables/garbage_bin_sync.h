// coop/interactables/garbage_bin_sync.h -- what a garbage container holds, and whether it can be picked up, are the
// host's on every peer.
//
// A garbage container (prop_garbageContainer_C, an open container) glues the props that settle inside it: each is
// frozen, attached to the container and listed in its `itemsInside`, and while that list is not empty the container
// cannot be held or collected (its checkPickup writes propData's canHold and canCollect). Tipped over, it lets them go.
// All of it runs in the container's tick, which a client never runs (coop/interactables/garbage_sync cancels it), so a
// client's copy showed an empty container that could be picked up while the host's held its contents. So:
//   - HOST: a poll sends each container's contents (the props' keys) and its two flags when they change, and every
//     container again to a joiner (GarbageBinState).
//   - CLIENT: writes the two flags on its copy, glues the props the host lists as the container's own glue does (frozen
//     by their setPropProps, attached, listed in `itemsInside`), and lets go of the ones it no longer lists as the
//     container's own release does (detached, awakeUnfreeze).
// Game thread.

#pragma once

namespace coop::net {
class Session;
struct GarbageBinPayload;
}  // namespace coop::net

namespace coop::garbage_bin_sync {

// Store the session and register the scan-hub consumer. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// The host's poll, or a client's apply of what it holds. Game thread.
void Tick();

// A client's receiver: the host's container.
void OnState(const coop::net::GarbageBinPayload& p);

// Every container again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::garbage_bin_sync
