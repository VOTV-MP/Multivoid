// coop/balance_sync.h -- shared host-authoritative credit balance (saveSlot.Points).
//
// The HOST owns the canonical balance. It POLLS Points each game-thread tick (catching
// every writer -- shop orders, signal-disk sells, task rewards, the +1000 dev button)
// and broadcasts BalanceSync on CHANGE (+ to a joining client on its connect edge), so
// every peer MIRRORS the host's balance. The CLIENT writes the host's absolute value
// directly (no AddPoints side-effects). HOST-authoritative, MTA server-economy shape.
// Game thread only for the apply paths (SetSession/Tick/connect run on the net-pump
// game thread; the receivers GT::Post their UObject writes).
//
// THE WIRE IS ONE-WAY, HOST->CLIENT (v135, security A5). A client->host BalanceDelta
// request lane existed from v30 and the host applied it via AddPoints with NO value
// bound, so any peer could set the shared balance to +/-2^31. It was retired whole
// rather than clamped (RULE 2): a clamp keeps a client-authored economy lane alive as a
// foothold and needs a "legitimate range" nobody can define. [V] Nothing legitimate used
// it -- its only in-tree sender was the +1000 dev button, already refused on a client by
// coop::dev_gate, and the laptop shop this header once named as a future user shipped on
// OrderRequest instead (coop/items/order_sync.h), where the HOST re-commits the order via
// makeAnOrder so the charge is host-authored like every other writer the poll catches.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::balance_sync {

void SetSession(coop::net::Session* session);

// Per-tick (net_pump game thread): on the HOST, poll Points + broadcast BalanceSync on
// change; on a CLIENT, retry applying the latest pending host balance until the saveSlot
// resolves (a connect-edge sync can arrive before the gamemode loads). No-op when not
// connected.
void Tick();

// HOST connect-edge: send the current balance to the just-joined client `slot` so it
// converges immediately (the change-based poll alone wouldn't fire if Points is static).
void OnClientConnect(int slot);

// RECEIVER (client): apply the host's ABSOLUTE balance -- write saveSlot.Points. No-op
// on the host (authoritative). GT::Posts the write.
void ApplyFromHost(int32_t total);

// Apply a LOCAL credit (the +1000 dev button) on the host or solo, via AddPoints. On a
// connected CLIENT this REFUSES and logs: there is no client->host economy write (the
// BalanceDelta lane was retired in v135, security A5). Safe from the render thread (menu).
void CreditLocal(int32_t amount);

// Reset the broadcast dedup on session teardown (so a reconnect re-broadcasts).
void OnDisconnect();

}  // namespace coop::balance_sync
