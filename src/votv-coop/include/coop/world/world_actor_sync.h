// coop/world/world_actor_sync.h -- the host-authoritative mirror of the non-character event
// actors (saucers, ships, the sky UFO, the space jellyfish) that fly in on events. The
// character-only NPC mirror cannot replicate them: it drives position, yaw-only rotation and
// the character movement component. So this is a self-contained sibling of the NPC sync: its
// own world-actor element (transform only, full rotation), its own mirror manager, its own
// wire (a reliable spawn and destroy, an unreliable pose batch), and its own second
// interceptor on the deferred spawn (the allowlist is disjoint from the NPC sync's, so the
// two never conflict). Host authoritative, since clients run a dormant event scheduler (the
// time sync pins their time scale to zero) and never fire an event: the host interceptor
// allocates an element and broadcasts the spawn, letting it proceed; the host post-observer
// binds the returned actor; the host destroy observer broadcasts the destroy and releases the
// element; the host pose tick publishes one pose batch; the client interceptor suppresses an
// allowlisted local spawn unless the bypass slot matches a wire materialisation; the client
// receivers materialise a transform-only mirror (deferred spawn, tick parked) and destroy
// it; the client tick drives every mirror; the connect broadcast re-sends live spawns.

#pragma once

namespace coop::net {
class Session;
struct WorldActorSpawnPayload;
struct EntityDestroyPayload;
}  // namespace coop::net

namespace coop::world_actor_sync {

// Resolve the allowlist classes and the deferred spawn path, and register the host observers
// and interceptor. Idempotent with retries (the classes load with the gameplay level).
// Caches the session for the receivers. Called every pump tick from the subsystems install.
void Install(coop::net::Session* session);

// True once the install attempt completed (it stops retrying), as the NPC sync's.
bool IsInstalled();

// Teardown: drain the mirrors (destroy the client ones, release-only the host ones) and clear
// the host reverse map and the bypass slot. Game thread.
void OnDisconnect();

// Host: read each live allowlisted actor's transform each tick and publish one pose batch for
// the net thread to fan out, so client mirrors move. Also the pose walk's dead-retire seam: a
// bound actor that reads dead closes its lifecycle here (the retire plus the destroy
// broadcast), since an event-end self-destroy is invisible to the destroy observer. A no-op
// on a client or a solo peer. Game thread.
void TickPoseStream();

// Client: drain the latest pose batch into an interpolation window per actor, then advance
// and drive every live mirror. A no-op on the host. Game thread.
void TickClientWorldActors();

// Host: re-send the spawn (the class and the current transform) for every already-spawned
// actor to a freshly connected client, so a joiner mirrors actors that spawned before it
// joined. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Client receiver: materialise a transform-only mirror for a host spawn. Game thread (the
// event drain posts it; the UFunction calls inside are game-thread only).
void OnWorldActorSpawn(const coop::net::WorldActorSpawnPayload& payload);

// Client receiver: tear down the mirror for a host destroy. Game thread.
void OnWorldActorDestroy(const coop::net::EntityDestroyPayload& payload);

// The host off-interceptor enrol for an already-spawned allowlisted actor the interceptor
// could not see (a blueprint-internal deferred spawn, caught by the world enumerator's
// source-gated thunk and drained here next pump tick, post-finish so the transform is real).
// Reaches the same end state as the interceptor and observer: the element, the actor bind,
// the reverse map and the spawn broadcast. Dedupes against the interceptor path via the
// reverse map. Returns the element id, or 0 if not enrolled. Game thread.
unsigned int HostEnrollExSpawn(void* actor);

// Who is a mirror, and when one is being born. Added for the coin-gun sync, which must decide,
// inside a component-overlap delegate, whether the coin it is looking at is a host-owned
// mirror (suppress: a client never credits) or genuine local content (leave native: two
// cooked maps carry placed coins, and cancelling those would make them permanently
// uncollectable and ghosted). Two calls because one is not enough: a mirror's
// component-bound delegates bind during begin-play, inside the finish spawn, before the
// mirror row is installed, so a row lookup alone loses the race for anything seeded onto a
// peer standing where the actor lands. Consumers test both.

// True if `actor` is a live wire-materialised mirror on this peer. Constant time. Any thread.
bool IsMirroredActor(void* actor);

// Register or unregister a materialised mirror. Called by the mirror path only. Any thread.
void NoteMirrorActor(void* actor, bool add);

// Drop every entry. Called from the teardown drain: the set is keyed on a raw pointer, so it
// must not outlive the mirrors (a recycled address would otherwise make a genuine local actor
// read as a mirror). Any thread.
void ClearMirrorActors();

// True while this thread is inside a mirror materialisation (the deferred begin through the
// finish, inclusive), exactly the window in which a mirror's begin-play runs and its
// delegates bind.
bool IsMaterializingMirror();

// The element id of the mirror this thread is materialising right now, or 0 outside the
// window. Knowing the window is open is not enough for a consumer that must name the actor
// being born: the collect lane suppressed a pickup on mirror-or-materialising and then
// forwarded on mirror alone, so a coin collected inside its own materialisation was cancelled
// and never forwarded, and neither peer credited it; the window carries the identity the
// mirror path already has. A fallback, never the first answer: the window belongs to the
// actor being spawned, and registering its collision can fire delegates on other,
// already-mirrored actors it lands on, so ask the actor for its own eid first and reach for
// this only when it has none and the window is open.
unsigned int MaterializingEid();

// The actor this thread is materialising, or null before the deferred begin has produced it
// (and outside the window). Published between the deferred begin and the finish, exactly
// the gap the actor exists in but its mirror row does not. This is what makes the window
// answerable about an actor: the window predicate takes no actor, so a consumer that ORs it
// into an is-this-a-mirror test is really asking whether some mirror is being born, and
// during any materialisation every unrelated coin, prop or pile a player touches answers
// yes. Compare against this instead.
void* MaterializingActor();

// Publish the actor into the open window. Called by the mirror path only, once, after the
// deferred begin returns and before the finish. A no-op outside a window.
void NoteMaterializingActor(void* actor);

// The RAII publisher for that window. Constructed by the mirror path around its spawn pair,
// with the eid it is about to install.
struct MaterializeScope {
    explicit MaterializeScope(unsigned int eid);
    ~MaterializeScope();
    MaterializeScope(const MaterializeScope&) = delete;
    MaterializeScope& operator=(const MaterializeScope&) = delete;

private:
    unsigned int prevEid_   = 0;
    void*        prevActor_ = nullptr;
};

}  // namespace coop::world_actor_sync
