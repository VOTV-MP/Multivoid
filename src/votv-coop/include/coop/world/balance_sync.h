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
// coop::dev_gate.
//
// CORRECTION 2026-08-24 (same day, security A34). This paragraph used to end "...and the
// laptop shop this header once named as a future user shipped on OrderRequest instead,
// where the HOST re-commits the order via makeAnOrder SO THE CHARGE IS HOST-AUTHORED like
// every other writer the poll catches." That was FALSE: `[V]` `makeAnOrder` contains no
// `addPoints` at all -- the charge lives in ui_laptop's Button_order ubergraph, which runs
// on the CLIENT before the order is ever forwarded. So the sentence asserted that the lane
// being retired already had a replacement, and it did not: the shop was free for every
// client. v136 supplies the real successor (the host prices the order from its own
// `list_store` and charges for it), and the retirement itself still stands.

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

// HOST: send the current balance to ONE slot, right now. Two callers, one reason -- the
// change-polled broadcast in Tick() only fires when the host's Points MOVES, so anything
// that needs a specific peer to converge while the value is STATIC has to say so directly:
//   * the connect edge (a joiner whose balance would otherwise stay wrong until the host
//     next spent or earned something);
//   * an order REFUSAL (v136, security A34) -- the host's balance does not move when it
//     refuses, yet the client has already debited itself locally through
//     `lib_C::addPoints`, which is EX_LocalVirtualFunction and cannot be suppressed. Without
//     this send the phantom debit persists until the host's balance happens to change.
// RENAMED from OnClientConnect 2026-08-24: the old name described ONE TRIGGER, which is how
// the refusal path came to be designed around a second function that did the same thing.
void SendCurrentToSlot(int slot);

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
