// coop/items/inventory_pickup_sync.h -- broadcast the inventory-collect blip so other peers hear a
// remote player pick an item up.
//
// The native cue is collector-only. On a successful collect the game plays inventory_Cue through
// PlaySound2D, which is 2D and reaches only the collecting client, and destroys the prop on the
// next statement; on the wire that collect is a bare PropDestroy, which carries no sound, so
// without this the pickup is silent for everyone else.
//
// Peer-symmetric and host-relayed, the firefly_sync shape: every peer post-observes
// GameplayStatics::PlaySound2D, keeps the calls that carry the collect cue at the collect pitch
// from the local player (the .cpp's predicate says what each test rules out), and broadcasts its
// own world position as InventoryPickup, wire 47. Receivers play the same cue spatialized there;
// the origin never receives its own send, so the collector still hears only the game's own 2D blip.
//
// Game thread only.

#pragma once

namespace coop::net { class Session; struct InventoryPickupPayload; }

namespace coop::inventory_pickup_sync {

// Idempotent; call from NetPumpTick. Resolves GameplayStatics::PlaySound2D +
// the param offsets + the inventory_Cue asset (throttled retry until found),
// then registers the POST observer once.
void Install(coop::net::Session* session);

// A peer collected an item -- play the blip at their broadcast position.
void OnReliable(const coop::net::InventoryPickupPayload& payload);

// Clear the session pointer (the observer stays registered; it self-gates).
void OnDisconnect();

}  // namespace coop::inventory_pickup_sync
