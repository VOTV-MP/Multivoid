// coop/players_registry.cpp -- see header for rationale.

#include "coop/players_registry.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/remote_player.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <string>

namespace coop::players {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

Registry& Registry::Get() {
    // Force-touch element::Registry FIRST so the destruction order is sane:
    // element::Registry constructed first -> destroyed last. Player Elements
    // owned here call element::Registry::FreeId in their destructors; if
    // element::Registry was destroyed first (default Meyers order would
    // destroy whichever was constructed last first), the FreeId would be UAF.
    // Touching the other singleton here forces our static-locals order.
    (void)coop::element::Registry::Get();
    static Registry s_instance;
    return s_instance;
}

void* Registry::RescanLocal() {
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (R::ClassNameOf(obj) != P::name::MainPlayerClass) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDO
        if (!R::IsLive(obj)) continue;
        // The discriminator: only the LOCAL player has a non-null Controller.
        // Puppets are explicitly unpossessed (AutoPossess + AI disabled at
        // deferred-spawn) per [[project-coop-enemies-target-both]]. This
        // check is the SINGLE place this discriminator lives -- everywhere
        // else uses Registry::IsLocal(actor) which queries the cache.
        if (!E::GetController(obj)) continue;
        return obj;
    }
    return nullptr;
}

void* Registry::Local() {
    if (localCached_ && R::IsLive(localCached_) && E::GetController(localCached_)) {
        return localCached_;
    }
    localCached_ = RescanLocal();
    return localCached_;
}

uint8_t Registry::LocalPeerId() const {
    return localPeerId_;
}

void Registry::SetLocalPeerId(uint8_t id) {
    // Idempotent on same id (caller pattern: host calls this every tick).
    if (localPeerId_ == id && id < kMaxPeers && playerBySlot_[id]) return;
    // If the local peer id changes (re-assign after reconnect), drop the
    // old slot's Element shadow first so its destructor frees the ElementId.
    if (localPeerId_ != id && localPeerId_ < kMaxPeers) {
        DropPlayerElement_(localPeerId_);
    }
    localPeerId_ = id;
    if (id < kMaxPeers) {
        // Create the LOCAL Player Element (puppet=nullptr; the local IS
        // the local player). Other slots' Player Elements track puppets
        // and are created by RegisterPuppet.
        EnsurePlayerElement_(id, /*puppet=*/nullptr);
    }
}

void Registry::InvalidateLocal() {
    localCached_ = nullptr;
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
    // Create or refresh the Player Element shadow for this peer.
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
    if (peerSlot >= kMaxPeers) return nullptr;
    return playerBySlot_[peerSlot].get();
}

void Registry::EnsurePlayerElement_(uint8_t peerSlot, coop::RemotePlayer* puppet) {
    if (peerSlot >= kMaxPeers) return;
    // Idempotent: if an Element already exists with the same (peerSlot, puppet)
    // signature, no-op. Callers like net_pump's host self-registration loop
    // re-invoke this every tick.
    if (auto* existing = playerBySlot_[peerSlot].get()) {
        if (existing->PeerSlot() == peerSlot && existing->Puppet() == puppet) {
            return;
        }
        // Mismatch: same slot but different puppet (e.g. local re-allocated
        // as a puppet, or vice versa). Drop the old before re-creating.
        DropPlayerElement_(peerSlot);
    }
    auto el = std::make_unique<coop::element::Player>(peerSlot, puppet);
    // Role-aware allocation (audit fix 2026-05-28): host range is reserved
    // for the authoritative side; client processes must allocate from the
    // peer range so client-local Player Elements don't collide with host-
    // allocated NPC/Prop ElementIds when the v12 protocol bump puts
    // ElementId on the wire. The host process always has localPeerId_=0
    // (kPeerIdHost); any other localPeerId_ value means this is a client
    // process. localPeerId_ == kPeerIdUnknown (0xFF) means role not yet
    // determined -- in that case treat as client (safer: peer range is
    // larger and never wire-authoritative; mistaken host-range allocation
    // would cause id collisions later).
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
    UE_LOGI("players::Registry: allocated Player Element eid=%u for peerSlot=%u "
            "(puppet=%p; local=%s; role=%s)",
            eid, peerSlot, puppet, puppet ? "no" : "yes",
            isHost ? "host" : "client");
}

void Registry::DropPlayerElement_(uint8_t peerSlot) {
    if (peerSlot >= kMaxPeers) return;
    if (!playerBySlot_[peerSlot]) return;
    const auto eid = playerBySlot_[peerSlot]->GetId();
    // unique_ptr reset -> destructor -> element::Registry::FreeId.
    // No shared lock with element::Registry; the destructor will acquire
    // element::Registry::m_mutex briefly. Safe here on the game thread.
    playerBySlot_[peerSlot].reset();
    UE_LOGI("players::Registry: released Player Element eid=%u for peerSlot=%u",
            eid, peerSlot);
}

}  // namespace coop::players
