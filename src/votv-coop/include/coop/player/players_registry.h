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
// the peer id of the local or any puppet. All object access on the game thread; lookups are
// cached pointers, with the initial scan on the first Local call and liveness re-validated per
// query, since the engine recycles actor slots on a level change.

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
    // a non-owning pointer and clears it on unregister, or when the actor goes away on a level
    // change.
    void RegisterPuppet(uint8_t peerSessionId, RemotePlayer* puppet);
    void UnregisterPuppet(uint8_t peerSessionId);

    // The identity queries.

    // The canonical is-this-the-local-player predicate, used instead of a controller check.
    bool IsLocal(void* actor);

    // True if `actor` is one of our spawned puppets, any peer.
    bool IsPuppet(void* actor);

    // The peer session id for `actor`, or unknown if not in the registry.
    uint8_t PeerIdOfActor(void* actor);

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

    // The Player Element shadows, one per peer slot, null if not allocated. Owned by the
    // registry: constructed by the puppet registration or the local id, destroyed by the
    // unregister or replaced by a new local id. Game thread only.
    std::unique_ptr<coop::element::Player> playerBySlot_[kMaxPeers];
};

}  // namespace coop::players
