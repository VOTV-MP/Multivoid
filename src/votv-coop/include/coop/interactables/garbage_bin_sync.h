// coop/interactables/garbage_bin_sync.h -- what a garbage container holds, and whether it can be picked up, are the
// host's on every peer.
//
// A garbage container (prop_garbageContainer_C) glues the props that settle inside it: each is frozen, attached and
// listed in its `itemsInside`, and while the list is not empty the container cannot be held or collected (checkPickup
// writes propData's canHold and canCollect). A client never runs that tick (coop/interactables/garbage_sync), so its copy
// showed an empty container that could be picked up. So:
//   - HOST: the add and propAwoken's clear each end in checkPickup, which the script gate watches (a POST): it marks the
//     container, and the death of a prop it listed, seen at the destroy seam, marks it too. One flush a tick sends each
//     marked container's contents (the props' keys) and flags when they differ from what the clients were told
//     (GarbageBinState); a joiner is sent every container at its replay.
//   - CLIENT: once every listed prop is here (a state waits for the key index to move, with no clock), writes the two flags,
//     glues the props the host lists that its own list lacks (as the container's glue does),
//     lets go of the ones its list holds that the host does not list (as its own release does), and writes the list.
// Game thread.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::garbage_bin_sync {

// Store the session and register the scan-hub consumer. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// Registers and settles the checkPickup watch; the host's flush and a joiner's retry, or a client's apply of what it holds.
// Game thread.
void Tick();

// HOST, from the destroy seam: a prop died; the container that listed it is sent again.
void NoteDestroyed(void* actor);

// One chunk of a GarbageBinState message arrived (game thread, in the drain): when the message is whole, a client keeps the
// host's state of a container until it can apply it whole, which waits for every listed prop to be here. A chunk shorter
// than a chunk is dropped and counted; every length is read from the assembled body, and what does not add up to it is
// dropped, counted and logged.
void OnChunk(coop::net::Session& session, const void* payload, size_t payloadLen, int senderPeerSlot);

// HOST: every container to the joining slot alone, at its connect replay. It runs at the next tick, once the scan index is
// current for this world (it waits for the index's own pass to complete, never sends nothing); what the transport does not
// take is asked again each tick.
void QueueConnectBroadcastForSlot(int slot);

// CLIENT: the host's states resolved (every listed prop here) since the session began, applied or, with garbage_state_off,
// skipped (the drills read it).
uint32_t ResolvedCount();

// CLIENT: the states actually applied since the session began (fewer than ResolvedCount with garbage_state_off; the drills
// read it).
uint32_t AppliedCount();

// A peer left: what it was still owed goes.
void OnPeerGone(uint8_t slot);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::garbage_bin_sync
