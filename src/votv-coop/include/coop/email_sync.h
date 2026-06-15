// coop/email_sync.h -- v64/v65: the meadow-PC EMAIL mirror (appends + deletes).
//
// USER ASK (2026-06-11): the laptop's messages -- the hash-collection task
// mails, the scientist/alien responses, the event mails from caught signals
// -- must mirror cross-peer.
//
// RE (votv-computers-phase2-impl-RE-2026-06-12.md SS3): every producer
// funnels through gamemode.addEmail (PE-invisible) into saveSlot.emails;
// gamemode.addEmail on the RECEIVER reproduces everything in one reflected
// call (persistence append + list row + the email ding at the physical
// laptop + tab highlight) and re-stamps the date from the host-synced clock.
// Deletion is ui_laptop.delEmail(Index) (the row's del button) -- mirrored
// since v65.
//
// Mechanism: every peer keeps a SHADOW of saveSlot.emails -- one entry per
// row holding (a) the POD instance key read raw off the array bytes (zero
// reflected calls at cadence; ue_wrap::email::RowKey) and (b) the row's
// cross-peer identity = FNV-1a 64 of its serialized blob (the exact
// EmailAppend bytes; date-free, so the per-peer addEmail re-stamp doesn't
// split identities). A 1 Hz poll diffs the array against the shadow
// positionally (the game only ever appends at the tail; deletes shift):
//   APPEND  -> serialize + chunk-broadcast (EmailChunkPayload, host-relayed;
//              per-row send-or-retry, audit I-3). Wire-applied rows are
//              recognized by instance key and adopt the WIRE hash as
//              identity (echo-proof + delete-key consistency even when the
//              receiver resolved a different pfp).
//   SHRINK  -> the removed rows' hashes broadcast as EmailDelete (v65);
//              receivers resolve their local index by hash and run the
//              native delEmail. Index-keyed deletes would be wrong: a
//              producer appends natively before its row relays, so peers
//              hold different array ORDERS under concurrent appends.
// A delete for a hash not (yet) present tombstones briefly: it lands when
// the row's chunked append completes (the delete-beats-append race) or
// TTLs out. World-down drops the shadow and re-primes silently at world-up
// (fresh allocations = fresh instance keys; never diff across a reload).
//
// Identity caveat (engineered, documented): the instance key is
// {topic/text FText data pointers, pfp pointer, date.X, username byte}.
// A false "kept" verdict needs BOTH freed FText allocations recycled at
// identical addresses for a content-different row stamped the same minute
// by the same sender within one poll second -- treated as negligible, same
// class as an FNV collision.
//
// Join: saveSlot.emails rides the v56 save transfer -- a joiner starts
// converged; this channel covers live appends/deletes only.
//
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::email_sync {

// Store the session. Idempotent; called per tick from subsystems::Install.
void Install(coop::net::Session* session);

// Per-tick: throttled resolve + the 1 Hz shadow poll (append broadcast +
// delete diff) + tombstone retry.
void Tick();

// Wire ingest: one chunk of an appended row (assembly + apply). Host
// additionally relays via the whitelist; both roles apply.
void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Wire ingest: one content-keyed delete. Applies via the native
// ui_laptop.delEmail (or tombstones if the row isn't here yet).
void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot);

// Aggregate teardown: drop shadow/assemblies/tombstones + reset seq.
void OnDisconnect();

}  // namespace coop::email_sync
