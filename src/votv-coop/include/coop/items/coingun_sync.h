// coop/items/coingun_sync.h -- the sell gun and its coins. Abaocoin_C derives from AActor, not
// Aprop_C, so no prop lane synced a coin: a client's sale spun up coins on its own machine and
// credited a local balance the host's next broadcast erased, while the sold prop's destroy
// replicated. The shape is a presser-authored outcome with a host-derived value: the client's
// verb cannot be cancelled, so its own coins are captured at the FinishSpawningActor POST and
// destroyed at the next net-pump barrier (never inside the verb's bracket), commit-or-release per
// shot; the client sends CoinGunSell naming the prop by save key, eid as the keyless fallback,
// ahead of its unchanged PropDestroy on the same lane, so the host handles the sale while its
// prop is alive; the host prices from its own copy, mints through the game's own
// prop_coingun_C::sell, destroys the sold prop itself and answers with CoinGunResult. The coins
// are host-owned world actors mirrored by world_actor_sync, and a body tripping the host's coin
// credits the host, a client's puppet included. Game thread unless a function says otherwise.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::coingun_sync {

// Resolves the verb, the two Func seams and the collect interceptor and caches the session;
// idempotent with retries (the gun and coin classes load with the level). Every pump tick.
void Install(coop::net::Session* session);

// The net-pump barrier on the client (destroys the coins captured inside the gun bracket) and the
// consumption-guard sweep on the host (about 1 Hz, drops sold-set entries whose prop has died).
// Cheap when idle.
void Tick();

// Host ingest of CoinGunSell; a no-op elsewhere. Authorisation first: the named prop must be within
// the sender's reach, measured on the host's copy of the sender's puppet (the gun traces from the
// sender's camera; the prop's bounds and the pose-staleness budget widen it), fail closed with no
// live puppet (TooFarAway, and the seller is told). Consumption: the host destroys the sold prop
// itself right after `sell`, through the ordinary destroy seam, and the seller's own PropDestroy
// lands as the no-op echo. The residual: the reach anchors on the sender's own reported position,
// which ValidatePose admits with no delta-per-time check, so a peer teleporting its pose to each
// prop can still sell it; the closure is a movement validator on the pose lane, not a second gate
// here. The eid is range-checked in both bands; the key resolves through the index only, since a
// key this peer never indexed is not a prop the sender could have shot and a cold walk per
// hostile packet is what an attacker would buy. A world with no gun is a throttled miss.
// `senderSlot` names the peer to log and answer; it selects no gun.
void OnReliable(const uint8_t* payload, int len, uint8_t senderSlot);

// Client ingest of CoinGunResult: one feed line telling the seller the price the host used, or why
// nothing was minted. A no-op on the host.
void OnReliableResult(const uint8_t* payload, int len);

// Host ingest of CoinCollect: a client collected a mirror of one of our coins, so the collect runs
// on the authoritative coin by dispatching its own actionOptionIndex, and the game's native credit
// and self-destroy follow. Checks the payload size, then fails closed on the element type and the
// actor's class. The eid is range-checked in the host band only: a baocoin_C is host-minted by
// construction, so no other band can name one, and without the gate the no-resolve branch's "the
// coin is already gone" would swallow a client-band eid or garbage as positive knowledge.
// Refusals are rate-latched. `localPlayer` is the host's own mainPlayer, the verb's `player`
// argument, which the credit block never reads; it never comes off the wire. Not null-guarded.
void OnCoinCollect(const uint8_t* payload, int len, uint8_t senderSlot, void* localPlayer);

// Session teardown: dumps the lane's counters, then drops every piece of world-scoped state (the
// sold set, the barrier queue, the cached gun). The resolved class, UFunction and CDO pointers
// are not world-scoped and stay.
void OnDisconnect();

// For a freshly materialised baocoin_C mirror: stops it simulating. Targets the coin's Sphere
// component by name, the one shipping bSimulatePhysics true, which is not the root (the collect
// sphere is declared first), so the actor-level helper would miss it and the mirror would fight
// the pose drive. Resolved on the declaring class, UPrimitiveComponent.
void PrepareCoinMirror(void* coin);

// The coin's birth value: baocoin_C's ReceiveBeginPlay picks the material from `points`, once
// (up to 10 bronze, 11 to 25 silver, 26 and above gold), and `sell` writes that int between
// BeginDeferredActorSpawnFromClass and FinishSpawningActor, so a mirror born at the CDO default
// of 5 rendered the wrong denomination, not a wrong tint. These three resolve off the coin's own
// class, which is resident whenever a coin is in hand.

// Whether a UClass is the coin: a pointer compare against the resolved class, a name compare
// before Install resolved it. Callers holding the UClass do not render its name (on the connect
// snapshot that cost one engine string per WorldActor element). Also the "carries a birth value"
// predicate for the WorldActor lane; when those stop being the same question, this changes.
bool IsCoinClass(void* cls);

// A live coin's `points`, or -1 when the property does not resolve, which callers report rather
// than use. Game thread.
int32_t ReadCoinPoints(void* coin);

// `points` onto a deferred-spawned mirror before FinishSpawningActor runs BeginPlay, the sequence
// `sell` performs; false if unresolvable. Valid only inside the deferred window. Game thread.
bool SeedCoinMirror(void* coin, int32_t points);

// The instrument: a coin's `points` and the material its BeginPlay painted, read off the component
// named `baocoin` (the bytecode names it at all three SetMaterial sites; the root is the collect
// sphere, and a reader aimed there returns null on both peers and agrees by construction). The
// material is independent evidence, painted by the game from the real value, so a bad read shows
// as a mismatch on one line. `material` is empty if the component or function does not resolve.
void DescribeCoin(void* coin, int32_t& outPoints, std::wstring& outMaterial);

// True while this thread is inside a playerHandUse_LMB dispatch whose context is a
// prop_coingun_C; the destroy seam asks it to put CoinGunSell in front of its own broadcast.
// Thread-local.
bool IsInCoinGunVerb();

// Called by the destroy seam on the client right before its ordinary PropDestroy for a prop killed
// inside the gun bracket: sends CoinGunSell with the same key-and-eid pair the destroy carries
// (an empty key means the eid names it). A no-op on the host, when not connected, or with neither
// name present. The seam calls this after its own gates, so the world-load episode and the
// reconcile window are inherited, and a joining client's loadObjects churn authors no sales.
void SendSaleForDyingProp(const std::wstring& key, uint32_t elementId);

}  // namespace coop::coingun_sync
