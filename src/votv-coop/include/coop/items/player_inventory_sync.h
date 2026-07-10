// coop/player_inventory_sync.h -- per-player client inventory (host-persisted, GUID-keyed).
//
// Replaces the v56 whole-host-save inventory inheritance with a Minecraft-style per-player
// inventory persisted on the HOST at SaveGamesDir()/<save_name>/coop_players/<guid>.json,
// keyed by the client's durable GUID (coop::config::ReadPlayerGuid, carried in the Join).
//
// Plan: research/findings/votv-inventory-impl-plan-2026-06-14.md. Design (Topic 2):
// votv-wisp-and-client-inventory-RE-2026-06-12.md.
//
// INCREMENT 1 (this file's current scope): identity is wired (Join carries the GUID), and the
// HOST ensures each joining peer's per-save inventory FILE exists (an empty-inventory
// placeholder). No inventory DATA is read/serialized/applied yet -- that is Increment 2
// (serializer + read layer), Increment 3 (client->host stream + host persist), and Increment 4
// (host->client apply-on-join, gated behind an SP probe). Each increment is added here as it
// lands; this header grows with them (one feature per file, modular cap honored).

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct BlobChunkPayload; }

namespace coop::player_inventory_sync {

// Cache the session pointer. Call once at boot (subsystems Install).
void Install(coop::net::Session* session);

// INCREMENT 3 -- transport + host persistence.

// Bidirectional PlayerInventoryBlob receiver (event_feed -> here); branches by role:
//   * HOST receiving from a CLIENT slot (1..): one chunk of that client's inventory STREAM.
//     Reassembles (per-sender) and, on a complete + CHANGED blob, persists it to that peer's
//     coop_players/<guid>.json (atomic, magic + FNV integrity, .bak of last good, 15s rate-limit).
//   * CLIENT receiving from the HOST (slot 0): one chunk of the host's ON-JOIN apply blob (Inc 4,
//     host->client). Reassembles and, on completion, deserializes + stashes it as the pending
//     per-player inventory (HasPendingApply()), to be written into the save object by the
//     SaveObjectReadyHook before the world materializes.
// Game thread.
void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderPeerSlot);

// INCREMENT 4 -- the live apply on join (host->client + the SaveObjectReadyHook).

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

// Per-tick. INCREMENT 2 scope: a one-shot READ-VERIFY self-test (ini inventory_selftest=1) --
// ~5s after world-up it reads the LOCAL saveSlot inventory via ue_wrap::inventory::ReadAll and
// logs the item counts + the first item's class/key/group sizes, proving the structural read
// works. (Increment 3 fills this with the real client->host inventory stream.) No-op unless
// the ini flag is set. Game thread.
void Tick();

// HOST-only: ensure peer `peerSlot`'s per-save inventory file exists. Builds
// <SaveGames>/<hostSlot>/coop_players/<guid>.json (guid = the Join-carried GUID for that
// slot); creates the coop_players/ dir + an empty-inventory placeholder file if absent.
// No-op off the host, or when the peer's GUID hasn't arrived yet (pre-v73 / Join not landed).
// Called at the host's connect-replay edge (subsystems ConnectReplayForSlot). Game thread.
void EnsurePlayerFile(int peerSlot);

}  // namespace coop::player_inventory_sync
