// coop/interactables/upgrade_sync.h -- the signal machine's upgrade levels, host-authoritative.
// Overview: docs/signals.md.
//
// TWO halves of one bug. The levels are a single struct on the save, so they rode only the
// transferred save and a level bought mid-session diverged in silence. And a client's purchase was
// client-local: it debited the CLIENT and raised the CLIENT's level, so the group paid nothing and
// the host's next balance broadcast handed the money back -- free upgrades for all but the host.
//
// The mirror is change-POLLED on the host rather than published at the purchase, because the panel
// is not the only writer: the server racks and the transformer write the same struct as physical
// upgrades, and three of its members have no panel row at all.
//
// The purchase is an intent. The client refuses its own button at the script-body gate, so it never
// debits itself and a refusal needs no correction; the host re-derives price and bounds from its
// own table and its own level. The .cpp carries the rest.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct UpgradeLevelsPayload;
struct UpgradeIntentPayload;
}  // namespace coop::net

namespace coop::upgrade_sync {

// Cache the session and arm the two button gates. Idempotent; the watch is retried from Tick
// until the widget class loads. Game thread.
void Install(coop::net::Session* session);

// Per-tick (net-pump game thread). HOST: poll the struct and broadcast on change, then run one
// queued purchase per sender per token. CLIENT: keep the gate enabled and retry the latest
// pending mirror until the store resolves (a join-edge mirror can arrive before the save loads).
void Tick(coop::net::Session& session);

// HOST: send the current levels to ONE slot, at its world-ready edge. The change poll fires only
// when the struct MOVES, so a joiner whose loaded save is older than the host's levels would stay
// wrong until the next purchase.
void SendCurrentToSlot(int slot);

// RECEIVER (client): take the host's absolute levels. The write is Tick's, which also repaints an
// open panel; this stashes them. No-op on the host.
void ApplyFromHost(const coop::net::UpgradeLevelsPayload& payload);

// RECEIVER (host): a client asks to buy or sell one level. Queued; Tick performs it.
void OnUpgradeIntent(coop::net::Session& session, const coop::net::UpgradeIntentPayload& payload,
                     uint8_t senderSlot);

// A peer left: drop its queue and its rate bucket.
void OnPeerLeft(uint8_t slot);

// Session teardown: drop the queues and the broadcast dedup, so a reconnect re-publishes.
void OnDisconnect();

}  // namespace coop::upgrade_sync
