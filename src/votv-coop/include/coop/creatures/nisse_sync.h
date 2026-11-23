// coop/creatures/nisse_sync.h -- the Nisse are the host's.
//
// The Christmas tree (prop_xmastree_C) mints three Nisse (tomtenisse_C) beside itself in its `woken`, when its player
// slept through midnight; bedEvent_C runs it on each machine for that machine's own player, so each player's Nisse
// were their own and the host's reached no one. HOST: the tree and the vengeful soul (nisse_soul_C) are catch sources,
// and the Nisse cross as world-actor mirrors. CLIENT: its tree's `woken` is cancelled and, when its own player slept
// through midnight, asked of the host (AnomalyAsk op 2, coop/world/anomaly_ask); the host marks its tree's night and
// runs its `woken`, whose own date and soul checks still decide. A client's swing at a Nisse is a hit intent
// (coop/creatures/hit_target_creature). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::nisse_sync {

inline constexpr const wchar_t* kNisseClass = L"tomtenisse_C";

void Install(coop::net::Session* session);

// The watch, retried until registered. Game thread, once a tick.
void Tick();

// HOST: a client's player woke with its tree's night marked (AnomalyAsk op 2).
void OnWokenAsk(uint8_t senderSlot);

void OnDisconnect();

}  // namespace coop::nisse_sync
