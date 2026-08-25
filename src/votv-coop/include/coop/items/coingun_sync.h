// coop/items/coingun_sync.h -- the SELL GUN and its COINS (v137, security A37/A38).
//
// ============================================================================================
// FIELD-FAILED 2026-08-24, the same day it shipped. READ THIS BEFORE TRUSTING ANY PROSE BELOW.
// The hands-on found FOUR failures and the design prose in this header contains claims that are
// now MEASURED FALSE. Roots + the corrected design + the build ladder:
//   research/findings/inventory-items/votv-v137-field-defects-DESIGN-2026-08-24.md
//
//   1. THE INTENT NEVER AUTHORS. -- FIXED IN v138 (B1). `CoinGunSell` named the prop by an ELEMENT
//      ID, and a v122 client mints no Element row for its OWN save-loaded keyed prop -- so the eid
//      was 0 for exactly the props a player shoots. 3 of 3 sales refused on the client. The
//      surviving identity is the KEY, which `PropDestroy` already carries and whose receiver
//      resolves; the payload now carries it, and the host resolves key-first / eid-fallback.
//      NOT hands-on confirmed -- see THE SALE'S IDENTITY below for what B1 does and does not close.
//   2. "A REFUSED SALE DEGRADES TO EXACTLY TODAY'S BEHAVIOUR" (below) IS FALSE BY CONSTRUCTION.
//      -- FIXED IN v140: the capture is per-shot and COMMIT-OR-RELEASE (see THE BARRIER QUEUE in
//      coingun_sync.cpp). A shot that authored no sale releases its coins instead of eating them.
//      The capture of the client's own coins is UNCONDITIONAL (it keys on the verb alone) while
//      the authorization is CONDITIONAL and decided LATER and INDEPENDENTLY -- so a refusal is a
//      TOTAL loss: item gone, coins gone, nothing credited. The invariant that was missing:
//      a local artifact must not be suppressed until the authoritative one is CONFIRMED.
//   3. `PrepareCoinMirror` NEVER WORKED (6/6 `SetSimulatePhysics unresolved`) -- FIXED IN v140
//      (item 10): resolved on the DECLARING class. It resolves on the
//      LEAF `USphereComponent` while `R::FindFunction` is EXACT-OWNER (reflection.cpp:468-481) and
//      the function is declared on `UPrimitiveComponent`. Every other site in the tree resolves it
//      on the declaring class. Consequence: a mirror coin keeps simulating, and once the pose
//      delta gate silences a RESTING host coin the mirror drifts away uncorrected -- the likeliest
//      reason a client cannot pick up the host's coins.
//   4. THE COLLECT SEAM IS THE WRONG ENTRY -- FIXED IN v139 (B2) + v140 (both entries observed, and
//      the interceptor now mirrors the game's own mainPlayer_C gate). `[V]` TWO entries reach the credit block
//      `ExecuteUbergraph_baocoin:441`: the overlap BndEvt hooked here, and `actionOptionIndex`
//      DIRECTLY -- the E-press, `EX_LocalVirtualFunction`, PE-invisible. Six real host-side
//      credits produced ZERO lines from this interceptor, so `OnCollectPre` has never been
//      observed firing at all. Do not treat it as proven.
//   5. `Points` IS NOT COSMETIC -- **STILL OPEN, this is B3.** `[V]` `ReceiveBeginPlay` picks the coin's MATERIAL from `points`
//      (<=10 bronze / 11..25 silver / >=26 gold), once, and the CDO default is 5 -- so a mirror
//      renders the WRONG DENOMINATION, not a wrong tint.
//
// Also measured while re-deriving this lane: the sold prop's destroy lives in the GUN's ubergraph
// (@1730, inside the `if sold` branch), NOT inside `sell` -- so a host that only calls `sell` mints
// coins and leaves the prop alive. That is security A50, and v140's arbiter now runs the destroy
// itself. And the coin's self-destroy is Func-VISIBLE but NOT CANCELLABLE:
// `ufunction_hook`'s entire API is `InstallPostHook`, and `K2_DestroyActor` is `EX_VirtualFunction`
// so ProcessEvent never sees it either.
// ============================================================================================
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
//   2. the client sends `CoinGunSell{key, eid}` -- the artifact's NAME and nothing else -- immediately
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
// THE SALE'S IDENTITY (v138, B1 -- the fix for field failure 1 above). The sale names the prop the
// SAME WAY the destroy riding right behind it on the same lane names it: the save KEY first, the
// ElementId as the keyless fallback. That is not a widening for safety, it is the correction of a
// measured falsehood -- v137 shipped eid-only naming and the eid is 0 by construction on a client
// for its own save-loaded keyed props, so the lane could never author at all.
//   - KEYED prop -> the key resolves through the maintained key index (prop_element_tracker's
//     ResolveLiveActorByKey, the same primitive the destroy receiver uses).
//   - KEYLESS family (trash clump / chip pile carry Key=None) -> the eid resolves.
//   - NEITHER present -> the client authors NO sale. There is nothing to name, so sending would
//     only manufacture a refusal.
// WHAT THIS DOES NOT CLOSE: a keyed prop the host genuinely does not have -- the two peers already
// disagreed about that prop before anyone fired. That is a pre-existing stable-ID divergence this
// lane EXPOSES rather than causes; it refuses with NoSuchProp, the seller is TOLD, and the residual
// is filed to the stable-ID thread. "Logged loudly" is not a fix and is not claimed as one.
//
// THE RESULT SENTENCE (v138, B1). The host answers every sale with `CoinGunResult`, addressed to
// the seller. A refusal MUST be said: the seller's prop is already gone from its own screen (the
// destroy is deliberately unchanged and still lands) and no coins appeared -- which is
// letter-for-letter the symptom the user reported. Shipping that silence as designed behaviour
// would be shipping the bug. A SUCCESS is not a redundant ack either: it carries the price the HOST
// derived, and `[V]` `getPriceMultiplier` is per-instance and divergent (prop_batts by energy,
// prop_food by uses/ripeness, prop_cementBag, prop_garbBagRoll), so the seller's own local `sell`
// toast can legitimately name a different number. The result makes that visible instead of leaving
// the player to trust a toast we know can lie. Precedent: `order_sync`'s refusal, which answers
// with both a feed line and a state repair.
//
// WHAT THE INTENT DELIBERATELY DOES NOT CARRY (`COOP_SYNCER_MODEL.md` §2b: "an intent may name WHAT,
// never WHAT IT COSTS"): no price, no coin count, no class, no gun id. `[V]` `sell` reads ZERO gun
// FIELDS, so WHICH `prop_coingun_C` executes cannot change the outcome -- naming the gun would be a
// field nobody could act on, and a client's gun is a hand-item display mirror with no world element.
// (`sell` is not world-agnostic though: `[V]` `EX_Self` is the WorldContextObject of every deferred
// coin spawn inside it, so the executor must be a LIVE, world-placed instance -- a CDO mints zero.)
//
// RESIDUAL, DELIBERATE AND LOGGED: `[V]` two cooked maps carry PLACED `baocoin_C` instances. Those
// are level content on both peers, never enrolled, never mirrors -- and a client picking one up with
// its OWN body is left native. Cancelling that would make them permanently uncollectable AND
// ghosted, which is a NEW loss; leaving it keeps today's behaviour, and the collect logs a line.
//
// The cancel is NO LONGER simply "mirror-scoped", and the older one-word summary is corrected here
// rather than left standing (v140): there are now three non-mirror cases the client DOES cancel, each
// for a reason that is not about mirrors at all -- our own pre-barrier coins (they are not level
// content and the host may be about to mint the real ones), any coin a PUPPET trips (a puppet IS a
// mainPlayer_C, so the native path would credit US for someone else's pickup), and nothing at all
// when the toucher is not a player, because `[V]` the game's own gate casts OtherActor to
// mainPlayer_C and credits nobody otherwise.
//
// Game-thread only unless a function says otherwise.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::coingun_sync {

// Resolve the verb + the two Func seams + the collect interceptor; cache the session. Idempotent
// with retries (the gun/coin BP classes load with the level). Called every NetPumpTick.
void Install(coop::net::Session* session);

// The NET-PUMP BARRIER (client) + the CONSUMPTION-GUARD SWEEP (host). On the client, destroys the
// coins captured inside the gun bracket (see THE BARRIER RULE). On the host, throttled to ~1 Hz,
// drops sold-set entries whose prop has died -- which is what makes the guard's lifetime a fact
// instead of the claim v137's comment made about a map that was erased nowhere. Cheap when idle.
void Tick();

// HOST ingest for `ReliableKind::CoinGunSell`. No-ops off the host.
//
// SECURITY A50 -- MITIGATED in v140 (2026-08-25), NOT CLOSED. Read the residual at the end of this
// block before writing "closed" anywhere. Two halves shipped:
//   - AUTHORIZATION. The receiver now asks one question about the ACTOR before any question that
//     spends the world: is the named prop within the SENDER'S OWN reach, measured on the host's copy
//     of the sender's puppet? `[V]` the gun traces `arm(1000.0)` from the sender's own CAMERA
//     (prop_coingun ubergraph [4] -> `mainPlayer.arm`, whose start is
//     `GetPlayerCameraManager().K2_GetActorLocation()`; an earlier wording said "FROM THE PLAYER",
//     which is true of the body but not of the origin, and the eye-height offset is one of the terms
//     `kPoseStalenessUU` absorbs), widened by the prop's measured bounds and that budget. MTA
//     `CUnoccupiedVehicleSync.cpp:244/491` `IsPointNearPoint3D` shape. FAIL-CLOSED: no live puppet on
//     the host means no body to measure from, so the sale is refused (`TooFarAway`) and the seller is
//     told, rather than a reach being assumed for a body we cannot see.
//   - CONSUMPTION. The host destroys the sold prop ITSELF, right after `sell` returns -- the native
//     order (`sell` at gun ubergraph [24], `K2_DestroyActor` at [26]; the destroy has never been
//     inside `sell`). Until v140 the cost half was delegated to the client's own PropDestroy arriving
//     behind us, i.e. to the attacker's goodwill, and the success log said so out loud. The destroy
//     rides E::DestroyActor -> Actor.K2_DestroyActor -> the ordinary prop_destroy_seam Func patch, so
//     every peer learns about it through the one existing mechanism; the seller's own PropDestroy
//     then lands as the steady-state no-op echo OnDestroyImpl_ already implements.
// *** THE RESIDUAL, AND IT IS WHY THIS ROW IS NOT CLOSED (audit CRITICAL C-1, 2026-08-25) ***
// The reach gate anchors on the SENDER'S OWN POSITION, and the sender writes it. `[V]` an inbound
// pose is admitted by `ValidatePose` alone -- a static garbage filter (finite, |xyz| <= 1e6 cm = TEN
// KILOMETRES, a SELF-REPORTED speed bound, angle ranges) with no delta-vs-time check anywhere -- and
// the one site that examines distance ACCEPTS the teleport, against a threshold scaled by that same
// self-reported speed. So the enumeration survives at one extra packet per prop: pose to the prop,
// sell the prop, repeat. What v140 genuinely changed is that the host now also DESTROYS each prop it
// pays for, so the attack costs the world its props instead of being free.
// THE CLOSURE IS A HOST-SIDE MOVEMENT VALIDATOR ON THE POSE LANE -- at the boundary where the
// unvalidated value enters, once, for every lane that will ever ask "was this peer near it" -- not a
// second compensation inside this receiver. Until that ships, this row reads MITIGATED.
//
// What this receiver still does NOT do, deliberately recorded rather than claimed away:
//   - nothing further on the eid: it IS range-checked (v140 item 6, both bands, for the reason the
//     PropDestroy receiver accepts both). An earlier sentence here said "fully range-checks the
//     payload" when nothing did, and its replacement then said the check was still owed after the
//     check had shipped -- the same lane's third and fourth false header sentences, in opposite
//     directions. This one is code-checked;
// SECURITY A51 -- CLOSED in v140 (2026-08-25). Both attacker-reachable GUObjectArray walks in this
// lane are gone:
//   - the key resolve is INDEX ONLY (`FindLiveActorByKey`, not `ResolveLiveActorByKey`). The cold
//     `FindByKeyString` fallback is a full walk with a key-string read per prop, and an
//     attacker-chosen key is a guaranteed miss, so every hostile packet bought one whole walk on the
//     host's game thread -- compounding inside a single frame, because the reliable inbox drains with
//     an unbounded `while (TryGetReliable(msg))` per tick. A key that is not indexed is not a prop
//     this peer can legitimately have shot: the client only knows the key because it mirrored the
//     prop from us, which required a Prop Element, which is what puts it in the index;
//   - `FindLiveGun` cached only its POSITIVE result, so a world with no gun in it re-walked on every
//     sale -- and `[V]` prop_coingun is placed in 3 of 261 maps, so that is the ordinary world. The
//     miss is now throttled to 250 ms.
// The unbounded per-tick reliable drain itself is NOT fixed here; it is a cross-lane property of
// event_feed and is recorded as such rather than patched from inside one receiver.
// Rows + evidence: `docs/security/TRACKER.md` 2026-08-25 block.
//
// `senderSlot` names the peer to log and to address the CoinGunResult back to -- it does NOT select a
// gun instance (FindLiveGun takes no argument and there is nothing to prefer; v137's comment here
// claimed otherwise and was false).
void OnReliable(const uint8_t* payload, int len, uint8_t senderSlot);

// CLIENT ingest for `ReliableKind::CoinGunResult`. Renders one feed line telling the seller what
// its sale did -- the price the host used, or why nothing was minted. No-ops on the host.
void OnReliableResult(const uint8_t* payload, int len);

// HOST ingest for `ReliableKind::CoinCollect` (v139, B2). A client collected a coin that mirrors one
// of ours; perform the collect on the authoritative coin by dispatching its OWN `actionOptionIndex`,
// so the game's native credit and self-destroy run. Checks the payload SIZE, then fails closed on the
// Element TYPE and the actor's CLASS. No-ops off the host.
//
// IT RANGE-CHECKS THE EID, **HOST BAND ONLY** (v140 item 6), and the asymmetry with the sale lane --
// which accepts either band -- is the point: `[V]` a baocoin_C is host-minted by construction (no save
// key, spawned by the host's own `sell`, announced under the host's own eid), so no other band can
// name one. It matters beyond idiom because the no-resolve branch below logs "the coin is already
// gone" as POSITIVE KNOWLEDGE, and without the gate that claim would also swallow a client-band eid,
// an out-of-range eid and pure garbage -- discarding the fact that the number was never a host eid
// before anyone reads it. Refusals here are rate-latched for the same reason the sale lane's are.
//
// (This sentence is the FOURTH generation of itself and the previous three were all wrong: it said
// "fully range-checks the payload" when nothing did, then said the check was owed after the check had
// shipped. Twice overstating, once understating. Verified against `coingun_collect.cpp`'s actual gate
// on 2026-08-25 -- `[[lesson-false-security-comment-worse-than-none]]`.)
//
// `localPlayer` is the host's own mainPlayer, passed as the verb's `player` argument. `[V]` (upgraded
// from `[RD]` 2026-08-25 by direct bytecode read) block @441 is `addPoints(points, self)` + format +
// `addHint` + `K2_DestroyActor`; `K2Node_Event_player` appears nowhere in it, so the value cannot
// change the outcome -- and a client cannot steer it, since it comes from `event_feed::Update` and
// never off the wire. Not null-guarded. Game thread.
void OnCoinCollect(const uint8_t* payload, int len, uint8_t senderSlot, void* localPlayer);

// Session teardown. Dumps the lane's counters (a session that ends having measured nothing is the
// failure `[[lesson-your-own-session-end-summary-is-a-free-measurement]]` exists to prevent), then
// drops every piece of WORLD-scoped state: the sold-set, the barrier queue and the cached gun.
// The resolved UClass/UFunction/CDO pointers are NOT world-scoped and stay (CLAUDE.md 4j).
void OnDisconnect();

// Called by the WorldActor mirror path for a freshly materialized `baocoin_C` mirror: stops it
// simulating. Targets the coin's `Sphere` component BY NAME -- `[V]` that is the one shipping
// bSimulatePhysics=True, and it is NOT the root (`collect` is declared first), so the actor-level
// helper would silently miss it and the mirror would fight the pose drive. Game thread.
void PrepareCoinMirror(void* coin);

// ---- v143 (B3): the coin's BIRTH VALUE ---------------------------------------------------------
// `[V]` `baocoin_C::ReceiveBeginPlay` -> `ExecuteUbergraph_baocoin(1441)` reads `points` and calls
// `baocoin.SetMaterial(0, ...)`: <=10 bronze @1499, >11 && <=25 silver @1725, >26 gold @1820. So the
// coin's colour is not something we paint -- it is something the GAME paints from one int, at
// BeginPlay, once. `[V]` `prop_coingun_C::sell` writes that int BETWEEN `BeginDeferredActorSpawn-
// FromClass` and `FinishSpawningActor` at three of its four mint sites; our client mirror had nothing
// in that window, so every mirrored coin was born at the CDO default of 5 and painted bronze.
//
// THESE THREE RESOLVE OFF `R::ClassOf(coin)`, NOT off the module's `CoinPointsOffset()` global.
// `Install()` early-returns forever once both lane latches set, and its `points` resolve is guarded on
// `g_coinClass` being resident -- `baocoin_C` loads on demand with the gun asset, so a session where
// it was not resident at latch time leaves that global at -1 PERMANENTLY. A producer reading -1 would
// send birthLen=0, the receiver would correctly leave the CDO alone, and the fix would silently do
// nothing all session while the log showed a benign "no birth content". The actor is in hand at every
// call site and its class is definitionally resident, so ask the class.

// Read a live coin's `points`. Returns -1 if the property cannot be resolved -- callers MUST treat
// that as "unknown, say so loudly", never as a value. Game thread.
int32_t ReadCoinPoints(void* coin);

// Write `points` onto a freshly BeginDeferred'd mirror, before FinishSpawningActor runs its
// BeginPlay. This is precisely the sequence `sell` itself performs. Returns false if unresolvable.
// Game thread, and ONLY valid inside the deferred window -- after FinishSpawning the branch has
// already run and writing the value would change nothing visible.
bool SeedCoinMirror(void* coin, int32_t points);

// The instrument: a coin's `points` AND the material its own BeginPlay actually painted, read off the
// component named `baocoin`. NOT the BP root (`collect`, a USphereComponent declared first) and NOT
// `Sphere` -- `[V]` the bytecode names `baocoin` at all three SetMaterial sites, and a reader aimed at
// the root returns null on BOTH peers and therefore AGREES BY CONSTRUCTION, which is an instrument
// that cannot fail. The material matters because producer and instrument read `points` through the
// same offset at the same site: a wrong read would print agreement while the coins still drew
// differently. The material name is INDEPENDENT evidence -- the game painted it from the real value,
// not from our read -- so a bad read surfaces as `points=5 mat=inst_baocoin_s` on one line.
// `material` is left empty if the component or the function cannot be resolved. Game thread.
void DescribeCoin(void* coin, int32_t& outPoints, std::wstring& outMaterial);

// TRUE while this thread is inside a `playerHandUse_LMB` dispatch whose Context is a
// `prop_coingun_C`. The destroy seam asks this to know that the prop it is watching die is the gun's
// victim, so it can put `CoinGunSell` in front of its own broadcast. Synchronous, thread-local.
bool IsInCoinGunVerb();

// Called by the destroy seam, ON THE CLIENT, immediately BEFORE it sends its ordinary PropDestroy
// for a prop killed inside the gun bracket. Sends `CoinGunSell{key, eid}` -- the SAME identity pair
// the destroy that follows carries, deliberately (see THE SALE'S IDENTITY). `key` empty means the
// prop is keyless and the eid names it. No-op on the host, when not connected, or when BOTH names
// are absent (nothing to name -> authoring a sale could only produce a refusal). The seam calls
// this AFTER all of its existing gates, so the world-load episode and the R-4a reconcile window are
// inherited -- which is what stops a joining client's `loadObjects` churn from authoring sales
// (principle 8).
void SendSaleForDyingProp(const std::wstring& key, uint32_t elementId);

}  // namespace coop::coingun_sync
