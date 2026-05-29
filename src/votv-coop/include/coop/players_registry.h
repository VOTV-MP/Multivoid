// coop/players_registry.h -- central source of truth for player identity.
//
// MTA-inspired (`reference/mtasa-blue/`'s CClientPlayerManager): all
// player-identity lookups go through ONE singleton registry instead of
// scattered `FindObjectByClass(MainPlayer)` scans that ambiguously
// return either the local mainPlayer_C or a puppet (since both share
// the same UClass).
//
// Why one registry, not one Player class (deviation from full MTA mirror):
// our `coop::RemotePlayer` carries puppet-specific state (Spawn, Tick,
// SetTargetPose, satellite Character pull, AnimBP rewiring, nameplate
// anchor, ping). The LOCAL player has NONE of that -- it's just an
// actor pointer + peerSessionId. Forcing a unified class would have
// `Player.Spawn()` no-op'd for the local, `Player.Tick()` ignored, etc.
// Splitting keeps the puppet machinery in `RemotePlayer` where it
// belongs and adds a thin `LocalEntry` to the registry for the local.
//
// Identity invariants (RULE 1 root-cause fix, 2026-05-26):
// - `Registry::Get().Local()` returns the actor pointer of the local
//   mainPlayer_C (the one with a non-null Controller) OR nullptr if
//   not yet alive.
// - `Registry::Get().Puppet(peerSessionId)` returns the RemotePlayer*
//   for that peer's puppet OR nullptr.
// - `Registry::Get().IsLocal(actor)` is the canonical "is this the
//   local player?" predicate. Use this instead of
//   `GetController(actor) != nullptr` everywhere.
// - `Registry::Get().PeerIdOfActor(actor)` -> the peer id of either
//   the local or any puppet matching the actor; -1 if unknown.
//
// All UObject access on the game thread. Lookups are O(1) via cached
// pointers; the initial scan for the local mainPlayer_C happens on
// first Local() call and re-validates via IsLive each query (the
// engine recycles actor slots on level change).

#pragma once

#include "coop/element/element.h"

#include <cstdint>
#include <memory>

namespace coop { class RemotePlayer; }
namespace coop::element { class Player; }

namespace coop::players {

// Peer session ids. Host=0; clients 1..(kMaxPeers-1). Each peer only
// ever has ONE local + up to (kMaxPeers-1) puppets (other peers'
// players). 2026-05-26 user direction: support at least 4 players
// total in coop. kMaxPeers = 4 covers host + 3 clients.
inline constexpr uint8_t kMaxPeers       = 4;
inline constexpr uint8_t kPeerIdHost     = 0;
inline constexpr uint8_t kPeerIdUnknown  = 0xFF;  // not in registry

class Registry {
public:
    // Singleton accessor. Thread-safe (the underlying static initializer
    // is C++17 thread-safe; no manual locking needed for construction).
    static Registry& Get();

    // ---- Local player ----

    // Returns the local mainPlayer_C actor pointer, or nullptr if not
    // yet alive. Cached + IsLive-validated; the underlying GUObjectArray
    // scan happens only when the cache is empty/stale. Game thread only.
    void* Local();

    // The peer session id assigned to THE LOCAL player on THIS process.
    // Set by Session at connect time (host=0, client=1). 0xFF until set.
    uint8_t LocalPeerId() const;
    void SetLocalPeerId(uint8_t id);

    // Force re-resolve of the local cache. Call on level change.
    void InvalidateLocal();

    // ---- Puppets (remote players) ----

    // Returns the RemotePlayer* for a given peer id, or nullptr. The
    // puppet's actor is `RemotePlayer::GetActor()`.
    RemotePlayer* Puppet(uint8_t peerSessionId);

    // Register / unregister a puppet. The RemotePlayer instance is
    // OWNED by the caller (typically harness.cpp's `g_orphan`); the
    // registry just holds a non-owning pointer + clears it on
    // UnregisterPuppet (or when the actor goes away on level change).
    void RegisterPuppet(uint8_t peerSessionId, RemotePlayer* puppet);
    void UnregisterPuppet(uint8_t peerSessionId);

    // ---- Generic identity queries ----

    // The canonical "is this actor the local player?" predicate. Use
    // this instead of GetController(actor) != nullptr.
    bool IsLocal(void* actor);

    // True if `actor` is one of our spawned puppets (any peer).
    bool IsPuppet(void* actor);

    // -> peer session id for `actor`, or kPeerIdUnknown if not in registry.
    uint8_t PeerIdOfActor(void* actor);

    // ---- Element shadow (Tier 3 Players migration 2026-05-28) ----

    // Returns the Player Element for this peer slot, OR nullptr if the slot
    // is empty (no puppet registered, no local set for this slot). The
    // Element's `GetId()` is the unified ElementId used for future cross-
    // subsystem addressing (event_feed dispatch, late-joiner snapshot, etc).
    // Game thread only.
    coop::element::Player* GetPlayerElement(uint8_t peerSlot);

    // Convenience: the LOCAL peer's Player Element id, or
    // coop::element::kInvalidId if not yet allocated (boot/seed window).
    // Use this to stamp `senderElementId` on outbound wire packets
    // (ItemActivate / Weather / RedSky / Lightning under v13). Safe to
    // call from any thread that touches the registry on the game thread;
    // intentionally a snapshot read with no internal locking (Element
    // ids are atomic-write on alloc / atomic-write on free, the worst
    // tearing window is the boot moment where a half-published Element
    // could carry kInvalidId, which the caller skips anyway).
    coop::element::ElementId LocalPlayerElementId() const;

    // (v14 LocalPlayerSyncContext / LocalPlayerIdentity were the per-peer
    // accessors for the 8-bit Element generation byte. v16 PR-FOUNDATION-1b
    // retired both -- per-peer stale-generation defense now lives in the
    // PacketHeader's senderEpoch field, stamped by Session at WriteHeader
    // and latched per peerSlot by Session::HandleMessage. Wire callers no
    // longer need any Element-context accessor at all.)

    // ---- A4 (2026-05-29) mirror exchange for wire-side ElementId ----

    // Wire-driven mirror creation. Called by receivers of the connect-edge
    // handshake (event_feed.cpp's AssignPeerSlot + Join handlers) once the
    // remote peer's local Player Element id is known. The receiver drops
    // any locally-allocated placeholder Player Element in `peerSlot` (the
    // one created by RegisterPuppet or SetLocalPeerId before the handshake
    // resolved the cross-peer id) and installs a MIRROR Player Element at
    // `wireEid` via coop::element::Registry::RegisterMirror.
    //
    // The puppet pointer for `peerSlot` is preserved (snapshot before
    // drop / restored after install) so PuppetByPeer_(peerSlot) still
    // returns the right RemotePlayer*. For the local slot's mirror call
    // (host eid received by the client), the puppet pointer stays nullptr
    // (the local has no puppet).
    //
    // Returns true on successful mirror install. Returns false if
    // wireEid is kInvalidId / 0 / out of range, or if Registry::RegisterMirror
    // failed (slot collision -- duplicate handshake or wire-id reuse bug
    // upstream). Game thread only.
    //
    // v14 added a `wireContext` parameter (handshake byte stamped onto the
    // mirror's Element::m_syncContext for downstream per-payload compare).
    // v16 PR-FOUNDATION-1b removed both the parameter and the underlying
    // Element-context machinery -- the mirror no longer carries any
    // generation state; stale-gen defense lives entirely in the packet
    // header's senderEpoch latched at the Session layer.
    bool EstablishMirrorForSlot(uint8_t peerSlot, coop::element::ElementId wireEid);

private:
    // Element shadow lifetime helpers (file-local; see .cpp).
    void EnsurePlayerElement_(uint8_t peerSlot, coop::RemotePlayer* puppet);
    void DropPlayerElement_(uint8_t peerSlot);

private:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Re-resolve the local cache by walking GUObjectArray for a
    // mainPlayer_C with a non-null Controller. Game thread only.
    void* RescanLocal();

    void* localCached_ = nullptr;       // local mainPlayer_C actor, cached
    uint8_t localPeerId_ = kPeerIdUnknown;
    // One puppet slot per peer id. Local is NOT in this array; it's
    // tracked via `localCached_` + `localPeerId_`. So if localPeerId_=1
    // (this peer is client #1), `puppetByPeer_[0]` is the host's puppet
    // on this peer's process, and slots [2], [3] would carry clients 2/3
    // (when N-peer scope expands beyond 1v1).
    RemotePlayer* puppetByPeer_[kMaxPeers] = {};

    // Player Element shadows -- one per peer slot. nullptr if not allocated.
    // Owned by the registry. Constructed by RegisterPuppet / SetLocalPeerId
    // (via EnsurePlayerElement_); destroyed by UnregisterPuppet / replaced
    // by SetLocalPeerId (via DropPlayerElement_). Game thread only.
    std::unique_ptr<coop::element::Player> playerBySlot_[kMaxPeers];
};

}  // namespace coop::players
