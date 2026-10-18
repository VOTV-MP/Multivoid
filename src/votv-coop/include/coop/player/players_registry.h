// coop/player/players_registry.h -- the central source of truth for player identity, MTA's
// player manager shape: every player-identity lookup goes through one registry instead of
// scattered class scans that ambiguously return either the local player or a puppet, since
// both share the class. One registry rather than one player class: RemotePlayer carries the
// puppet-specific state (spawn, tick, target pose, the satellite character pull, the animation
// rewiring, the nameplate anchor, ping), and the local player has none of that, just an actor
// pointer and a peer id; a unified class would have most of it no-op for the local. The
// invariants: Local returns the local player's actor (the one with a controller) or null;
// Puppet returns the peer's RemotePlayer or null; IsLocal is the canonical
// is-this-the-local-player predicate, used instead of a controller check; PeerIdOfActor gives
// the peer id of the local or any puppet; IsPuppetBody is IsPuppet plus a puppet's ragdoll display
// body, and SetPuppetCountListener tells one consumer when the number of puppets changes. All object
// access on the game thread; lookups are cached pointers, with the initial scan on the first Local
// call and liveness re-validated per query, since the engine recycles actor slots on a level change.

#pragma once

#include "coop/element/element.h"
#include "ue_wrap/core/cached_obj_ref.h"

#include <cstdint>
#include <memory>

namespace coop { class RemotePlayer; }
namespace coop::element { class Player; }

namespace coop::players {

// Peer session ids: the host is 0, clients count up from 1. Each peer has one local and up to
// the maximum minus one puppets; the maximum covers a host and three clients.
inline constexpr uint8_t kMaxPeers       = 4;
inline constexpr uint8_t kPeerIdHost     = 0;
inline constexpr uint8_t kPeerIdUnknown  = 0xFF;  // not in registry

class Registry {
public:
    // The singleton accessor; the static initializer is thread-safe.
    static Registry& Get();

    // The local player.

    // The local player's actor, or null if not yet alive. Cached and liveness-validated; the
    // object-array scan runs only when the cache is empty or stale. Game thread only.
    void* Local();

    // The peer session id assigned to the local player on this process, set by the session at
    // connect; unknown until set.
    uint8_t LocalPeerId() const;
    void SetLocalPeerId(uint8_t id);

    // The puppets, the remote players.

    // The RemotePlayer for a peer id, or null; the puppet's actor is its GetActor.
    RemotePlayer* Puppet(uint8_t peerSessionId);

    // Register or unregister a puppet. The RemotePlayer is owned by the caller; the registry holds
    // a non-owning pointer and clears it on unregister. A puppet is registered by its spawn, before
    // its BeginPlay (RemotePlayer::Spawn), and unregistered by the caller after its destroy. The
    // local slot is refused: the local peer's Element never takes a puppet.
    void RegisterPuppet(uint8_t peerSessionId, RemotePlayer* puppet);
    void UnregisterPuppet(uint8_t peerSessionId);

    // Forget the slot's puppet pointer, and the slot's Player Element's copy of it: the Element
    // itself stays (the mirror the peer's packets resolve to), and a retried spawn's RegisterPuppet
    // rebinds it. For a spawn that registered its puppet and then failed to finish. Game thread only.
    void ClearPuppet(uint8_t peerSessionId);

    // The identity queries.

    // The canonical is-this-the-local-player predicate, used instead of a controller check.
    bool IsLocal(void* actor);

    // True if `actor` is one of our spawned puppets, any peer.
    bool IsPuppet(void* actor);

    // The live bodies of every player this process has: the local player's and each spawned puppet's, up to `max`,
    // into `out`; returns how many. What a trace that asks about the world, and not about the people in it, ignores.
    // Game thread only.
    int LiveBodies(void** out, int max);

    // The peer session id for `actor`, or unknown if not in the registry.
    uint8_t PeerIdOfActor(void* actor);

    // True if `actor` is a puppet's actor (IsPuppet) or the ragdoll display body of a registered puppet
    // (RemotePlayer::RagdollBody, non-null, and live at its recorded object-array index, so a recycled
    // address reads false). False for null. The puppet actor arm is a pointer compare, as IsPuppet; the
    // ragdoll arm also checks liveness. The extent is the puppet actor and its ragdoll display body: a
    // vehicle the puppet drives (the ATV) or a prop it carries is NOT a body, as MTA tests a vehicle as its
    // own element (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2505-2510).
    bool IsPuppetBody(void* actor);

    // The peer session id of the puppet whose actor or ragdoll display body `actor` is, or unknown.
    // Unlike PeerIdOfActor it does not answer for the local player.
    uint8_t PeerIdOfBody(void* actor);

    // Called with the number of registered puppets; a plain function, no context.
    using PuppetCountFn = void (*)(int count);

    // Install the one listener for the puppet count (a single consumer; a second would turn this
    // into a list). Calls `fn` once, immediately, with the current count, so a consumer set up after
    // puppets exist starts in sync, then after each RegisterPuppet / UnregisterPuppet / ClearPuppet
    // that changes the count of registered puppets. A null `fn` is stored and never called. Returns the
    // previous listener so a caller can put it back. The listener runs on the game thread and can fire
    // inside a puppet's deferred spawn: RegisterPuppet runs in the before-finish callback, before the
    // actor's BeginPlay. It must be O(1) and must not call the registry (no Register, Unregister or Clear:
    // the notify has no re-entry latch). A second setter displaces the first without telling it; a restore
    // of the returned listener re-syncs it through the set-time call. Game thread only.
    // Divergence: MTA pulls a count (reference/mtasa-blue/Server/mods/deathmatch/logic/ASE.cpp:166) and fires
    // onPlayerJoin after the join completes (CGame.cpp:1499); Source has a one-slot hook
    // (reference/source-sdk-2013/src/game/server/gameinterface.cpp:245,268). Here a push, fired at the
    // registry write inside the deferred spawn, because the consumer must arm before the puppet's first overlap.
    PuppetCountFn SetPuppetCountListener(PuppetCountFn fn);

    // The Element shadow.

    // The Player Element for this peer slot, or null if the slot is empty (no puppet registered,
    // no local set for it). Its id is the unified element id used for cross-subsystem addressing.
    // Game thread only.
    coop::element::Player* GetPlayerElement(uint8_t peerSlot);

    // The local peer's Player Element id, or the invalid id if not yet allocated (the boot and
    // seed window); stamped as the sender id on outbound wire packets. A snapshot read with no
    // internal locking: element ids are atomic-write on allocation and free, and the worst
    // tearing window is the boot moment where a half-published Element could carry the invalid
    // id, which the caller skips anyway.
    coop::element::ElementId LocalPlayerElementId() const;

    // The mirror exchange for the wire-side element id.

    // Wire-driven mirror creation, called by the receivers of the connect-edge handshake once the
    // remote peer's local Player Element id is known. The receiver drops any locally allocated
    // placeholder Player Element in the slot (the one created by the puppet registration or the
    // local id before the handshake resolved the cross-peer id) and installs a mirror Player
    // Element at the wire eid through the element Registry. The puppet pointer for the slot is
    // preserved across the drop and install, so the puppet lookup still returns the right
    // RemotePlayer; for the local slot's mirror (the host eid received by the client) the puppet
    // pointer stays null. True on a successful install; false if the wire eid is invalid, 0 or
    // out of range, or the Registry's register failed (a slot collision: a duplicate handshake,
    // or a wire-id reuse bug upstream). Game thread only.
    bool EstablishMirrorForSlot(uint8_t peerSlot, coop::element::ElementId wireEid);

private:
    // The Element shadow lifetime helpers; see the .cpp.
    void EnsurePlayerElement_(uint8_t peerSlot, coop::RemotePlayer* puppet);
    void DropPlayerElement_(uint8_t peerSlot);

    // The number of non-null puppet slots, and the listener call when it differs from `before`.
    int PuppetCount_() const;
    void NotifyPuppetCount_(int before);
    // The slot whose puppet's actor or non-null ragdoll body is `actor`, or kMaxPeers.
    uint8_t SlotOfBody_(void* actor) const;

private:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Re-resolve the local cache by walking the object array for a player with a non-null
    // controller. Game thread only.
    void* RescanLocal();

    // The local player's actor, cached across frames (menu windows included), a CachedObjRef
    // rather than a bare pointer with a liveness check. The reference also carries the world
    // stamp: the cache goes stale by itself when the world changes, checked at the point of use,
    // so no invalidation edge has to be noticed.
    ue_wrap::CachedObjRef localCached_;
    // The negative-result TTL for Local: at the menu, with no gameplay world, the cache misses
    // deterministically, and per-tick callers (the pump, the nameplates) would re-walk the object
    // array every frame, which once ballooned a client waiting in the menu by gigabytes. A miss
    // is cached for the TTL; world-up detection is delayed by at most that, irrelevant against a
    // multi-second world load. 0 means no cached miss.
    static constexpr unsigned long long kLocalMissTtlMs = 500;
    unsigned long long localMissAtMs_ = 0;
    uint8_t localPeerId_ = kPeerIdUnknown;
    // One puppet slot per peer id. The local is not in this array; it is tracked by the cached
    // reference and the local peer id. So on client 1, slot 0 is the host's puppet on this
    // process, and the other slots carry the other clients.
    RemotePlayer* puppetByPeer_[kMaxPeers] = {};
    PuppetCountFn puppetCountListener_ = nullptr;

    // The Player Element shadows, one per peer slot, null if not allocated. Owned by the
    // registry: constructed by the puppet registration or the local id, destroyed by the
    // unregister or replaced by a new local id. Game thread only.
    std::unique_ptr<coop::element::Player> playerBySlot_[kMaxPeers];
};

}  // namespace coop::players
