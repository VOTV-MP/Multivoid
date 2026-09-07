// coop/email_sync.h -- the meadow-PC EMAIL mirror: the laptop's messages (the hash-collection
// task mails, the scientist and alien responses, the event mails from caught signals) appear on
// every peer, and a row deleted on one disappears on all. Reader-facing summary: docs/devices.md.
// Every producer funnels through `gamemode.addEmail`, which is Blueprint-internal, into
// `saveSlot.emails`; that same reflected call on a RECEIVER reproduces everything at once --
// persistence append, list row, the ding at the physical laptop, tab highlight -- and re-stamps the
// date from the host-synced clock. The only remover, pak-wide, is the player's own
// `ui_laptop.delEmail(Index)`, which is why our peer-action feed line, "<nick> deleted an email",
// can never misattribute one.
// Appends are HOST-AUTHORED, deletes are peer-symmetric, and what a delete carries is a content
// hash rather than an index. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::email_sync {

// Store the session. Idempotent; called per tick from subsystems::Install.
void Install(coop::net::Session* session);

// --- the ready-edge join seed ---
//
// `saveSlot.emails` rides the join save transfer, but a row authored DURING the joiner's 30-60
// second load window is in neither the transferred save nor any later diff, and was silently
// never delivered. The seed closes that window: the host captures the array at the transfer's
// blob instant and sends the difference on the joiner's ready edge. HOST, game thread,
// per-slot, consume-once; the shared helper is `coop/session/join_seed`. Hooked beside the
// meadow lane's calls in save_transfer (capture and cancel) and in
// subsystems::ConnectReplayForSlot (seed).
void CaptureJoinSnapshot(int peerSlot);
void CancelJoinSnapshot(int peerSlot);
void QueueConnectBroadcastForSlot(int peerSlot);

// Slot teardown (roster row transition): drop the leaver's half-assemblies +
// seed bracket so a recycled occupant can never inherit them.
void OnDisconnectSlot(int peerSlot);

// Per-tick: the throttled resolve, the 1 Hz shadow poll (append broadcast + delete diff) and the
// tombstone retry.
//
// The shadow holds one entry per row: the POD instance key read raw off the array bytes (zero
// reflected calls at cadence, `ue_wrap::email::RowKey`) and the row's cross-peer identity, FNV-1a
// 64 of its serialized blob. The identity is date-free, so the per-peer re-stamp cannot split it.
// The poll diffs the array against the shadow POSITIONALLY, which the game permits because it only
// ever appends at the tail and deletes shift. An APPEND serializes and chunk-broadcasts, per row,
// send-or-retry. A SHRINK broadcasts the removed rows' hashes as EmailDelete. Index-keyed deletes
// would be wrong: a producer appends natively before its row relays, so peers hold different array
// ORDERS under concurrent appends. World-down drops the shadow and re-primes silently at world-up,
// since fresh allocations mean fresh instance keys and the diff must never cross a reload.
void Tick();

// Wire ingest: one chunk of an appended row (assembly + apply). The append is HOST-AUTHORED --
// every addEmail site is world, story or system authored, so a client is not an email authority --
// and this kind is therefore not relayed: a chunk arriving at the host from a client slot is a
// protocol violation, dropped at the dispatch so one client-side regression cannot pollute the
// shared inbox. A client still keeps the whole shadow and diff, which the mirror needs; only its
// send is gated.
//
// A wire-applied row is recognized by instance key and adopts the WIRE hash as its identity, which
// keeps it echo-proof and its delete key agreeing even when the receiver resolved a different pfp.
// The key is {topic and text FText data pointers, pfp pointer, date.X, username byte}, and its one
// engineered caveat is accepted: a false "kept" verdict needs BOTH freed FText allocations recycled
// at identical addresses, for a content-different row stamped in the same minute by the same
// sender, within one poll second -- the same negligible class as an FNV collision.
void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Wire ingest: one content-keyed delete, applied through the native `ui_laptop.delEmail`. A
// delete for a hash not yet present tombstones briefly: it lands when the row's chunked append
// completes -- the delete-beats-append race -- or it TTLs out.
void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot);

// Aggregate teardown: drop shadow/assemblies/tombstones + reset seq.
void OnDisconnect();

}  // namespace coop::email_sync
