// coop/items/player_inventory_sync.h -- the per-player profile (coop/items/player_profile.h:
// what a player carries, wears and holds, their vitals, where they stood), host-persisted at <game dir>/coop_players/<host save>/<guid>.json. The GUID is derived from the
// key that peer PROVED at admission (coop/net/peer_identity.h), never from a value it sent.
//
// A joiner's world is built from a capture of the HOST's save object, and the carried items
// live in that object (saveSlot.GObjStack[0]), so without a substitution every joiner carries
// the host's items under the host's keys. In session order: the host sends a joining peer its
// profile while that peer is still pre-world (a first join gets a starter kit under fresh
// keys), and the client writes it into the save object before the world materialises; the
// client streams its profile back as it changes, only from a world that was built from one;
// the host holds each complete, changed blob by GUID and cuts the held profiles to disk when its
// own world is saved, at no other moment (coop/player/player_profile_store.h says why). Nothing
// held and no file IS the first-join test.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct BlobChunkPayload; }

namespace coop::player_inventory_sync {

// Cache the session pointer and register the two save seams. Called at every session start.
void Install(coop::net::Session* session);

// ---- transport and host persistence ----

// Bidirectional PlayerInventoryBlob receiver (event_feed -> here); branches by role:
//   * HOST receiving from a CLIENT slot (1..): one chunk of that client's inventory STREAM.
//     Reassembles (per-sender) and hands a complete blob to the profile store, which holds
//     the newest one per GUID.
//   * CLIENT receiving from the HOST (slot 0): one chunk of the host's ON-JOIN apply blob.
//     Reassembles and, on completion, deserializes + stashes it as the pending
//     per-player inventory (HasPendingApply()), to be written into the save object by the
//     SaveObjectReadyHook before the world materializes.
// Game thread.
void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderPeerSlot);

// ---- the live apply on join: host to client, then the save-object-ready hook ----

// HOST: send peer `peerSlot` its per-player profile (the one the store holds, else its file,
// FNV-verified with a .bak fallback; the starter kit when there is neither -- never another
// player's inventory). Chunked to that ONE slot over PlayerInventoryBlob. Called from the host tick the
// moment the slot is connected and its GUID has arrived, so the blob lands in the joiner's
// pre-world window. No-op off the host or when the peer's GUID hasn't arrived. Returns true iff
// the blob was actually enqueued -- the caller (HostPersistTick) latches "sent" ONLY on true, and retries next tick on a channel-busy refusal
// (else the connect-edge refusal was never retried -> the client never got its inventory). Game thread.
bool SendInventoryToSlot(int peerSlot);

// CLIENT: true once the host's on-join apply blob has arrived + deserialized (the join boot waits
// on this before loading the world, so the SaveObjectReadyHook always has the data). Game thread.
bool HasPendingApply();

// CLIENT: the next save object to come ready belongs to a join, so the hook may substitute the
// profile into it (or empty the host's items out of it when none arrived). One-shot, consumed by
// the hook and cleared on disconnect; the join boot calls it right before each world load. Every
// other load in the process -- a later Host-with-save above all -- is left alone. Any thread.
void BeginJoinApply();

// CLIENT: where the join's world appearance puts this player, taken once: a later body in the same
// session is a respawn, which belongs at the start point. ProfilePose fills x, y, z and yaw with where
// the applied profile says the player stood. AtHost (the dev row join_at_host) leaves the player where
// the transferred save put it, the host's own position. StartPoint on a first join, for a player who
// left dead, with no profile applied, and for every later body. Game thread.
enum class JoinPlacement : uint8_t { StartPoint, ProfilePose, AtHost };
JoinPlacement TakeJoinPlacement(float& x, float& y, float& z, float& yaw);

// Per-slot disconnect (host): re-arm the on-join push for that slot. The leaver's profile stays
// held by GUID. Client: no-op. Game thread.
void OnDisconnectForSlot(int peerSlot);

// Aggregate disconnect: the client clears its send-dedup and any pending apply. The host keeps
// what the store holds -- on a host this edge is the last client leaving. Game thread.
void OnDisconnect();

// Per-tick: the client's outbound inventory stream, or the host's on-join push, by role. It
// also carries a one-shot read-verify self-test (ini inventory_selftest=1) that reads the local
// saveSlot inventory a few seconds after world-up and logs what it found; that part is a no-op
// unless the flag is set. Game thread.
void Tick();

}  // namespace coop::player_inventory_sync
