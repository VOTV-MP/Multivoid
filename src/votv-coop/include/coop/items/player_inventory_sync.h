// coop/items/player_inventory_sync.h -- a per-player client inventory, host-persisted and
// keyed by GUID.
//
// Each player keeps their own inventory instead of inheriting the host save's, in the
// Minecraft shape: the HOST persists it at SaveGamesDir()/<save>/coop_players/<guid>.json,
// keyed by the client's durable GUID -- derived from the key that peer PROVED at admission
// (coop/net/peer_identity.h), never from a value it sent.
//
// The four halves, in the order a session uses them: the host makes sure a joining peer's
// file exists; the host sends that peer its saved inventory at the connect replay, which the
// client stashes and writes into the save object before the world materialises; the client
// streams its inventory back as it changes; the host persists each complete, changed blob.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct BlobChunkPayload; }

namespace coop::player_inventory_sync {

// Cache the session pointer. Call once at boot (subsystems Install).
void Install(coop::net::Session* session);

// ---- transport and host persistence ----

// Bidirectional PlayerInventoryBlob receiver (event_feed -> here); branches by role:
//   * HOST receiving from a CLIENT slot (1..): one chunk of that client's inventory STREAM.
//     Reassembles (per-sender) and, on a complete + CHANGED blob, persists it to that peer's
//     coop_players/<guid>.json (atomic, magic + FNV integrity, .bak of last good, 15s rate-limit).
//   * CLIENT receiving from the HOST (slot 0): one chunk of the host's ON-JOIN apply blob.
//     Reassembles and, on completion, deserializes + stashes it as the pending
//     per-player inventory (HasPendingApply()), to be written into the save object by the
//     SaveObjectReadyHook before the world materializes.
// Game thread.
void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderPeerSlot);

// ---- the live apply on join: host to client, then the save-object-ready hook ----

// HOST: send peer `peerSlot` its persisted per-player inventory (read from coop_players/<guid>.json,
// FNV-verified, .bak fallback, EMPTY on missing/corrupt -- fail-safe, never leaks another player's
// inventory). Chunked to that ONE slot over PlayerInventoryBlob. Called at the host's connect-replay
// edge (subsystems ConnectReplayForSlot), right after EnsurePlayerFile. No-op off the host or when
// the peer's GUID hasn't arrived. Returns true iff the blob was actually enqueued -- the caller
// (HostPersistTick) latches "sent" ONLY on true, and retries next tick on a channel-busy refusal
// (else the connect-edge refusal was never retried -> the client never got its inventory). Game thread.
bool SendInventoryToSlot(int peerSlot);

// CLIENT: true once the host's on-join apply blob has arrived + deserialized (the join boot waits
// on this before loading the world, so the SaveObjectReadyHook always has the data). Game thread.
bool HasPendingApply();

// Per-slot disconnect (host): flush that peer's last inventory blob to disk + drop its
// in-memory entry. Client: no-op. Game thread.
void OnDisconnectForSlot(int peerSlot);

// Aggregate disconnect: host flushes all pending blobs; client clears its send-dedup. Game thread.
void OnDisconnect();

// Host shutdown hook: flush every connected peer's last blob to disk BEFORE the session stops
// (pure file I/O on captured bytes -- safe on the WM_CLOSE thread). No-op off the host.
void FlushAllToDisk();

// Per-tick: the client's outbound inventory stream, or the host's persist pass, by role. It
// also carries a one-shot read-verify self-test (ini inventory_selftest=1) that reads the local
// saveSlot inventory a few seconds after world-up and logs what it found; that part is a no-op
// unless the flag is set. Game thread.
void Tick();

// HOST-only: ensure peer `peerSlot`'s per-save inventory file exists. Builds
// <SaveGames>/<hostSlot>/coop_players/<guid>.json (guid = the Join-carried GUID for that
// slot); creates the coop_players/ dir + an empty-inventory placeholder file if absent.
// No-op off the host, or while the peer's GUID has not arrived (its Join has not landed).
// Called at the host's connect-replay edge (subsystems ConnectReplayForSlot). Game thread.
void EnsurePlayerFile(int peerSlot);

}  // namespace coop::player_inventory_sync
