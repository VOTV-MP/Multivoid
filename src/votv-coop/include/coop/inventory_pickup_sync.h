// coop/inventory_pickup_sync.h -- broadcast the inventory-collect BLIP
// (inventory_Cue, the "Half-Life pickup" sound) so OTHER peers hear a remote
// player collect an item.
//
// WHY (hands-on 2026-06-11 "still no half life pick up sound effect" +
// research/findings/votv-inventory-pickup-seam-RE-2026-06-11.md): the native
// blip is `PlaySound2D(inventory_Cue, 1.0, 1.1)` inside mainPlayer_C::
// putObjectInventory2 @659 -- 2D and COLLECTOR-ONLY by design, and a remote
// collect reaches other peers only as a bare PropDestroy -> structurally
// silent. Every BP-side call in the collect chain is EX_LocalVirtualFunction
// (ProcessInternal, invisible to our ProcessEvent detour); the ONE observable
// statement is the blip call itself: EX_Context(Default__GameplayStatics) +
// EX_FinalFunction(PlaySound2D) -- the same dispatch family as the shipped
// BeginDeferred/FinishSpawning observers.
//
// SHAPE (the firefly_sync precedent, PEER-SYMMETRIC + host-relayed):
//   - Every peer POST-observes UGameplayStatics::PlaySound2D, gated on
//       Sound == inventory_Cue   (allocation-free NameEquals)
//       1.05 < PitchMultiplier < 1.2   (collect = 1.1; dream-wake 0.9 and
//                                       customWall 2.0 are rejected -- the
//                                       game-wide cue census has ZERO other
//                                       matches at this pitch)
//       WorldContextObject == the LOCAL player (the collector; a puppet can
//                                       never dispatch it -- no input stack)
//     and broadcasts its own world position (InventoryPickup=47, v58).
//   - Receivers play the cue spatialized at that position via
//     coop::prop_sound::PlayInventoryBlipAt (vol 1.0 / pitch 1.1 /
//     att_default). No echo: the origin never receives its own send.
//
// Fires exactly once per SUCCESSFUL collect (single success-path block;
// all 26 putObjectInventory2 call sites game-wide funnel into it).
// PlaySound2D dispatches at human-event rate game-wide; the observer body is
// three cached-offset reads + compares.
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
