// coop/items/coingun_sync.h -- the SELL GUN and its COINS (v137, security A37/A38).
//
// THE DEFECT (register A13's worst instance). `[V]` `Abaocoin_C : public AActor` -- NOT an `Aprop_C`
// -- so it failed `IsDescendantOfProp` and appeared in no allowlist: coins were synced by nothing.
// A client firing the gun therefore span up coins only on its own machine, credited only its own
// local `saveSlot.points` (the pickup runs `lib_C::addPoints`, which is `EX_LocalVirtualFunction`
// and unhookable), and the host's next balance broadcast erased that number -- WHILE the sold prop's
// destroy replicated normally. Net effect, by default and without anyone attacking anything: the
// group loses the item and gains nothing.
//
// THE SHAPE: presser-authored OUTCOME, host-derived VALUE. The client's verb is uncancellable
// (`playerHandUse_LMB` is `EX_LocalVirtualFunction`: invisible to ProcessEvent AND to a Func patch,
// and `vm_dispatch` observes without a cancel primitive), so we do NOT try to stop it. Instead:
//
//   1. the client's own coins are CAPTURED at the `FinishSpawningActor` POST and destroyed at the
//      next net-pump barrier -- never in the bracket, see THE BARRIER RULE below;
//   2. the client sends `CoinGunSell{propEid}` -- an element id AND NOTHING ELSE -- immediately
//      BEFORE its ordinary, byte-UNCHANGED `PropDestroy`, on the SAME lane;
//   3. the host resolves that eid against ITS OWN world, prices the sale from ITS OWN copy via
//      `lib_C::sellObject`, and mints through the game's own `prop_coingun_C::sell`;
//   4. the coins are host-owned world actors, mirrored by the existing `world_actor_sync` lane;
//   5. whoever's body trips the HOST's coin credits the host -- the host's own body directly, a
//      client's PUPPET because `[V]` the pickup's entire gate is `DynamicCast(OtherActor,
//      mainPlayer_C)` with no locality test, and our puppets ARE `mainPlayer_C`.
//
// WHY ORDERING IS THE WHOLE DESIGN. The sale rides in front of the client's unchanged destroy on one
// FIFO lane, so the host handles it while its prop is still alive -- which the mint REQUIRES, because
// `[V]` `sell` positions its coins from the SOLD PROP's component (`rnd(c, comp)`), not from the gun.
// Leaving the destroy alone is what makes the failure path safe: if the host refuses for ANY reason
// the destroy still lands, which is EXACTLY today's behaviour. Nothing new is lost, so no heal lane
// exists to get wrong. An earlier draft suppressed the destroy and needed a re-express to repair the
// hole it opened; that is deleted.
//
// THE BARRIER RULE (`[[lesson-vm-bracket-zero-engine-mid-verb]]`). Our `FinishSpawningActor` POST
// fires MID-BYTECODE inside the still-open `playerHandUse_LMB` bracket. That window permits reads
// ONLY -- a nested ProcessEvent pump there corrupts. Independently, `[V]` `sell` applies
// `RandomUnitVector * f` as an impulse to the coin AFTER Finish returns and then loops to mint more,
// so destroying in the POST would also be a use-after-destroy. We therefore capture a
// world-stamped `CachedObjRef` and destroy at the barrier. Accepted cost: the client's coins live
// 1-2 frames and take their impulse, so there is a brief flash before the mirrors arrive.
//
// WHAT THE INTENT DELIBERATELY DOES NOT CARRY (`COOP_SYNCER_MODEL.md` §2b: "an intent may name WHAT,
// never WHAT IT COSTS"): no price, no coin count, no class, no gun id. `[V]` `sell` reads ZERO gun
// state, so WHICH `prop_coingun_C` executes cannot change the outcome -- naming the gun would be a
// field nobody could act on, and a client's gun is a hand-item display mirror with no world element.
//
// RESIDUAL, DELIBERATE AND LOGGED: `[V]` two cooked maps carry PLACED `baocoin_C` instances. Those
// are level content on both peers, never enrolled, never mirrors -- so the client-side collect cancel
// is MIRROR-SCOPED and leaves them native. Cancelling them would make them permanently uncollectable
// AND ghosted, which is a NEW loss; leaving them keeps today's behaviour, and the collect logs a line
// saying so.
//
// Game-thread only unless a function says otherwise.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::coingun_sync {

// Resolve the verb + the two Func seams + the collect interceptor; cache the session. Idempotent
// with retries (the gun/coin BP classes load with the level). Called every NetPumpTick.
void Install(coop::net::Session* session);

// The NET-PUMP BARRIER. Destroys the coins captured inside the client's gun bracket (see THE BARRIER
// RULE). No-op on the host and when nothing was captured.
void Tick();

// HOST ingest for `ReliableKind::CoinGunSell`. Fully range-checks the payload and no-ops off the
// host. `senderSlot` is used only for logging and for preferring that peer's gun instance.
void OnReliable(const uint8_t* payload, int len, uint8_t senderSlot);

// TRUE while this thread is inside a `playerHandUse_LMB` dispatch whose Context is a
// `prop_coingun_C`. The destroy seam asks this to know that the prop it is watching die is the gun's
// victim, so it can put `CoinGunSell` in front of its own broadcast. Synchronous, thread-local.
bool IsInCoinGunVerb();

// Called by the destroy seam, ON THE CLIENT, immediately BEFORE it sends its ordinary PropDestroy for
// a prop killed inside the gun bracket. Sends `CoinGunSell{eid}`. No-op on the host, on an invalid
// eid, or when not connected. The seam calls this AFTER all of its existing gates, so the world-load
// episode and the R-4a reconcile window are inherited -- which is what stops a joining client's
// `loadObjects` churn from authoring sales (principle 8).
void SendSaleForDyingProp(uint32_t elementId);

}  // namespace coop::coingun_sync
