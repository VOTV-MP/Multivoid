// coop/player/players_registry.cpp -- see coop/player/players_registry.h.

#include "coop/player/players_registry.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>  // GetTickCount64 (the Local() negative-miss TTL clock)

#include <atomic>
#include <string>

namespace {
// The local slot's ElementId, published through an atomic: the AssignPeerSlot stamp in
// session_status runs on the net thread, and reading the slot's unique_ptr from there is a
// data race. Net-thread readers (LocalPlayerElementId from the stamp path) get a lock-free
// coherent value.
std::atomic<coop::element::ElementId>
    g_localPlayerElementIdAtomic{coop::element::kInvalidId};
}  // namespace

namespace coop::players {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

Registry& Registry::Get() {
    // Touch the element registry first, so the destruction order is sane: constructed first,
    // destroyed last. Player Elements owned here free their ids in their destructors, and with the
    // default static-local order the element registry could be gone by then.
    (void)coop::element::Registry::Get();
    static Registry s_instance;
    return s_instance;
}

void* Registry::RescanLocal() {
    // An allocation-free walk: the class is matched by pointer, primed on the first textual hit,
    // and names are compared against the per-thread scratch, since two strings per scanned object
    // was the allocation bomb behind every cold rescan in a no-player window. The primed class
    // persists across rescans (a game-thread static); a blueprint class dies on world unload and
    // its address can be recycled, so it is revalidated (live and still named mainPlayer_C)
    // before each walk trusts it.
    static ue_wrap::CachedObjRef sMpClass;
    // The name deref is reached only after the slot-validated Alive short-circuits, never as a
    // blind deref of a freed class.
    if (sMpClass.Raw() && (!sMpClass.Alive() ||
                           !R::NameEquals(R::NameOf(sMpClass.Raw()), P::name::MainPlayerClass))) {
        sMpClass.Reset();
    }
    void* mpClass = sMpClass.Raw();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        if (mpClass) {
            if (cls != mpClass) continue;
        } else {
            if (!R::NameEquals(R::NameOf(cls), P::name::MainPlayerClass)) continue;
            mpClass = cls;
            sMpClass.Set(cls);  // fresh from this walk
        }
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // skip CDO
        if (!R::IsLive(obj)) continue;
        // World currency, and it fails closed, unlike the cached reference: for an actor an
        // undeterminable world is not a legitimate state, and being wrong here costs one more walk
        // half a second later, against feeding a dead world's controller to the engine for a whole
        // load. An immediate re-walk after a cache invalidation would find the same dead pawn,
        // still slot-live with an intact controller read, so invalidation alone could not fix that.
        // With no current world at all the term is skipped.
        if (void* const curWorld = ue_wrap::world_identity::CurrentWorld()) {
            if (ue_wrap::world_identity::WorldOf(obj) != curWorld) continue;
        }
        // The discriminator: only the local player has a non-null controller. Puppets are
        // unpossessed (auto-possess and the AI controller disabled at deferred spawn). The single
        // place the discriminator lives; everything else asks IsLocal, which reads the cache.
        if (!E::GetController(obj)) continue;
        return obj;
    }
    return nullptr;
}

void* Registry::Local() {
    // Warm-cache validation by identity, not possession: the local player never becomes a puppet,
    // so alive and not a registered puppet is the stable invariant for a cached pointer. A
    // non-null controller is not re-required here: a possessable local (driving the ATV, sitting
    // on a kerfur) hands its controller to the vehicle pawn yet is still the local player, and
    // re-requiring it invalidated the cache every frame while seated, so every frame fell through
    // to the full array walk. The cold rescan still uses the controller as the tie-breaker among
    // the mainPlayer instances.
    if (localCached_.Alive() && !IsPuppet(localCached_.Raw())) {
        return localCached_.Raw();
    }
    // The negative-result TTL: a miss is deterministic while no gameplay world is up, and a dead
    // cached pawn mid-travel misses for the whole load window, so per-tick callers must not
    // re-walk the array at tick rate. Worst case the new pawn is detected kLocalMissTtlMs late,
    // against a multi-second load.
    localCached_.Reset();
    const unsigned long long now = ::GetTickCount64();
    if (localMissAtMs_ != 0 && now - localMissAtMs_ < kLocalMissTtlMs) {
        return nullptr;
    }
    localCached_.Set(RescanLocal());  // fresh from the walk (Set(nullptr) == Reset)
    localMissAtMs_ = localCached_.Raw() ? 0 : now;
    return localCached_.Raw();
}

uint8_t Registry::LocalPeerId() const {
    return localPeerId_;
}

void Registry::SetLocalPeerId(uint8_t id) {
    // Reads and, through the drop and ensure paths, mutates the slot map; game thread only. The
    // callers are the pump's host self-registration each tick and the AssignPeerSlot handler.
    UE_ASSERT_GAME_THREAD("players::Registry::SetLocalPeerId (playerBySlot_)");
    // Idempotent on the same id; the host calls this every tick.
    if (localPeerId_ == id && id < kMaxPeers && playerBySlot_[id]) return;
    // A changed local id (a re-assign after a reconnect) drops the old slot's Element first, so
    // its destructor frees the id.
    if (localPeerId_ != id && localPeerId_ < kMaxPeers) {
        DropPlayerElement_(localPeerId_);
    }
    localPeerId_ = id;
    // A client now knows its slot, so its exclusive id band is activated before the local Player
    // Element is allocated; otherwise the allocation would mint from the pre-slot band and could
    // collide with another client's pre-slot id on the host relay. Slot 0 is skipped: the host
    // allocates from the host range.
    if (id != kPeerIdHost && id < kMaxPeers) {
        coop::element::Registry::Get().SetLocalPeerBand(id);
    }
    if (id < kMaxPeers) {
        // The local Player Element, with no puppet; other slots' Elements track puppets and are
        // created by RegisterPuppet.
        EnsurePlayerElement_(id, /*puppet=*/nullptr);
    }
}

RemotePlayer* Registry::Puppet(uint8_t peerSessionId) {
    if (peerSessionId >= kMaxPeers) return nullptr;
    return puppetByPeer_[peerSessionId];
}

void Registry::RegisterPuppet(uint8_t peerSessionId, RemotePlayer* puppet) {
    if (peerSessionId >= kMaxPeers) {
        UE_LOGW("players::Registry: peerSessionId %u out of range (max=%u)",
                peerSessionId, static_cast<unsigned>(kMaxPeers));
        return;
    }
    puppetByPeer_[peerSessionId] = puppet;
    // Create or refresh the Player Element for this peer.
    EnsurePlayerElement_(peerSessionId, puppet);
    UE_LOGI("players::Registry: registered puppet peerId=%u -> %p", peerSessionId, puppet);
}

void Registry::UnregisterPuppet(uint8_t peerSessionId) {
    if (peerSessionId >= kMaxPeers) return;
    puppetByPeer_[peerSessionId] = nullptr;
    DropPlayerElement_(peerSessionId);
}

bool Registry::IsLocal(void* actor) {
    if (!actor) return false;
    return actor == Local();
}

bool Registry::IsPuppet(void* actor) {
    if (!actor) return false;
    for (int i = 0; i < kMaxPeers; ++i) {
        if (puppetByPeer_[i] && puppetByPeer_[i]->GetActor() == actor) return true;
    }
    return false;
}

uint8_t Registry::PeerIdOfActor(void* actor) {
    if (!actor) return kPeerIdUnknown;
    if (actor == Local()) return localPeerId_;
    for (int i = 0; i < kMaxPeers; ++i) {
        if (puppetByPeer_[i] && puppetByPeer_[i]->GetActor() == actor) {
            return static_cast<uint8_t>(i);
        }
    }
    return kPeerIdUnknown;
}

coop::element::Player* Registry::GetPlayerElement(uint8_t peerSlot) {
    // The slot map is game-thread-only by convention; the net-thread-safe read is the atomic
    // behind LocalPlayerElementId.
    UE_ASSERT_GAME_THREAD("players::Registry::GetPlayerElement (playerBySlot_)");
    if (peerSlot >= kMaxPeers) return nullptr;
    return playerBySlot_[peerSlot].get();
}

coop::element::ElementId Registry::LocalPlayerElementId() const {
    // A lock-free read for non-game threads (the net-thread AssignPeerSlot stamp); published from
    // the ensure and drop paths under the game-thread invariant.
    return g_localPlayerElementIdAtomic.load(std::memory_order_acquire);
}

bool Registry::EstablishMirrorForSlot(uint8_t peerSlot,
                                       coop::element::ElementId wireEid) {
    // Reads and writes the slot map, game thread only; the callers are the handshake handlers,
    // dispatched from event_feed.
    UE_ASSERT_GAME_THREAD("players::Registry::EstablishMirrorForSlot (playerBySlot_)");
    if (peerSlot >= kMaxPeers) {
        UE_LOGW("players::Registry: EstablishMirrorForSlot peerSlot=%u out of range "
                "(max=%u) -- dropping",
                static_cast<unsigned>(peerSlot), static_cast<unsigned>(kMaxPeers));
        return false;
    }
    if (wireEid == coop::element::kInvalidId || wireEid == 0u) {
        UE_LOGW("players::Registry: EstablishMirrorForSlot peerSlot=%u "
                "wireEid=0x%08x is invalid sentinel -- dropping",
                static_cast<unsigned>(peerSlot), wireEid);
        return false;
    }
    // Idempotent: a slot already carrying a mirror with this id is a no-op, since a reconnect
    // edge can legitimately re-fire the assignment.
    if (auto* existing = playerBySlot_[peerSlot].get()) {
        if (existing->GetId() == wireEid && existing->IsMirror()) {
            return true;
        }
    }
    // The puppet pointer the old Element carried is kept, so the new mirror preserves the binding
    // for the puppet lookups; the local slot's is always null.
    coop::RemotePlayer* puppet = nullptr;
    if (auto* existing = playerBySlot_[peerSlot].get()) {
        puppet = existing->Puppet();
    }
    // Drop the locally allocated placeholder, which returns its id to the free list through the
    // destructor.
    DropPlayerElement_(peerSlot);
    // Build the mirror, then register it at the wire id: install in the slot map first, then
    // RegisterMirror; on failure drain the slot, so the destructor sees an invalid id and returns
    // without touching the registry.
    auto mirror = std::make_unique<coop::element::Player>(peerSlot, puppet);
    coop::element::Player* raw = mirror.get();
    playerBySlot_[peerSlot] = std::move(mirror);
    auto& reg = coop::element::Registry::Get();
    if (!reg.RegisterMirror(wireEid, raw)) {
        UE_LOGW("players::Registry: EstablishMirrorForSlot peerSlot=%u "
                "RegisterMirror(0x%08x) failed -- dropping placeholder",
                static_cast<unsigned>(peerSlot), wireEid);
        // A slot collision or out of range: drain the slot, so the failed mirror's destructor
        // early-returns with no double free.
        playerBySlot_[peerSlot].reset();
        return false;
    }
    // Defensive: if the local slot is ever mirrored (not done today, since this is called for
    // remote slots only), publish the wire id into the atomic, as the ensure path does.
    if (peerSlot == localPeerId_) {
        g_localPlayerElementIdAtomic.store(wireEid,
                                            std::memory_order_release);
    }
    UE_LOGI("players::Registry: established MIRROR Player Element eid=0x%08x "
            "for peerSlot=%u (puppet=%p; isHostRange=%s)",
            wireEid, static_cast<unsigned>(peerSlot), puppet,
            coop::element::Registry::IsHostId(wireEid) ? "yes" : "no");
    return true;
}

void Registry::EnsurePlayerElement_(uint8_t peerSlot, coop::RemotePlayer* puppet) {
    // Mutates the slot map and publishes the atomic: the publish is the net-thread-safe read side,
    // the mutation is game-thread-only.
    UE_ASSERT_GAME_THREAD("players::Registry::EnsurePlayerElement_ (playerBySlot_)");
    if (peerSlot >= kMaxPeers) return;
    // Idempotent: an Element with the same slot and puppet is a no-op; the pump's host
    // self-registration re-invokes this every tick.
    if (auto* existing = playerBySlot_[peerSlot].get()) {
        if (existing->PeerSlot() == peerSlot && existing->Puppet() == puppet) {
            return;
        }
        // Mirror Player Elements are wire-authoritative, bound to the sender's id. If the handshake
        // established a mirror before RegisterPuppet ran, dropping and reallocating here would
        // destroy the wire binding and the next packet's lookup by sender id would fail; the puppet
        // is bound in place instead.
        if (existing->IsMirror()) {
            existing->SetPuppet_(puppet);
            UE_LOGI("players::Registry: bound puppet=%p to existing MIRROR "
                    "Player Element eid=0x%08x peerSlot=%u",
                    puppet, existing->GetId(), static_cast<unsigned>(peerSlot));
            return;
        }
        // A mismatch, the same slot with a different puppet: drop the old before re-creating.
        DropPlayerElement_(peerSlot);
    }
    auto el = std::make_unique<coop::element::Player>(peerSlot, puppet);
    // Role-aware allocation: the host range is reserved for the authoritative side, and a client
    // allocates from the peer range so its Player Elements do not collide with host-allocated ids
    // on the wire. The host process always has local id 0; an unknown id (the role not yet
    // determined) is treated as a client, the safer choice, since the peer range is never
    // wire-authoritative.
    const bool isHost = (localPeerId_ == kPeerIdHost);
    auto& reg = coop::element::Registry::Get();
    const coop::element::ElementId eid =
        isHost ? reg.AllocHostId(el.get())
               : reg.AllocLocalId(el.get());
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("players::Registry: element::Registry::%s returned kInvalidId "
                "for peerSlot=%u -- Player Element not registered",
                isHost ? "AllocHostId" : "AllocLocalId", peerSlot);
        return;
    }
    playerBySlot_[peerSlot] = std::move(el);
    // Publish the local slot's id to the atomic, so net-thread readers see it lock-free.
    if (peerSlot == localPeerId_) {
        g_localPlayerElementIdAtomic.store(eid, std::memory_order_release);
    }
    UE_LOGI("players::Registry: allocated Player Element eid=%u for peerSlot=%u "
            "(puppet=%p; local=%s; role=%s)",
            eid, peerSlot, puppet,
            puppet ? "no" : "yes", isHost ? "host" : "client");
}

void Registry::DropPlayerElement_(uint8_t peerSlot) {
    // Mutates the slot map and clears the atomic; game thread only. The destructor briefly takes
    // the element registry's mutex.
    UE_ASSERT_GAME_THREAD("players::Registry::DropPlayerElement_ (playerBySlot_)");
    if (peerSlot >= kMaxPeers) return;
    if (!playerBySlot_[peerSlot]) return;
    const auto eid = playerBySlot_[peerSlot]->GetId();
    // The atomic is cleared before the destructor fires, so a concurrent net-thread reader cannot
    // observe an id about to be freed. Only the local slot is published.
    if (peerSlot == localPeerId_) {
        g_localPlayerElementIdAtomic.store(coop::element::kInvalidId,
                                            std::memory_order_release);
    }
    // The reset runs the destructor, which frees the id under the element registry's mutex,
    // briefly.
    playerBySlot_[peerSlot].reset();
    UE_LOGI("players::Registry: released Player Element eid=%u for peerSlot=%u",
            eid, peerSlot);
}

}  // namespace coop::players
