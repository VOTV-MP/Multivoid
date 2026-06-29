// coop/players_registry.cpp -- see header for rationale.

#include "coop/player/players_registry.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/hot_path_guard.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>  // GetTickCount64 (the Local() negative-miss TTL clock)

#include <atomic>
#include <string>

namespace {
// A4 audit finding 2026-05-29 (CRITICAL #1): the AssignPeerSlot stamp in
// coop/net/session_status.cpp::HandleConnStatusChanged runs on the GNS net
// thread, not the game thread. Reading through the non-atomic
// `unique_ptr<Player>` in `playerBySlot_[localPeerId_]` from that thread
// is data-race UB. Publish the local-slot ElementId through this atomic
// so net-thread readers (notably LocalPlayerElementId() called from the
// AssignPeerSlot stamp path) get a lock-free coherent value.
//
// v16 PR-FOUNDATION-1b: previously this atomic held a packed (eid:32,
// ctx:8) pair because senderContext stamping needed both fields atomically
// (single load -> coherent tuple, vs two-load tear during a game-thread
// DropPlayerElement_ race). v16 retired the ctx half (stale-generation
// defense moved to the packet header's senderEpoch); this atomic now
// holds the ElementId alone.
std::atomic<coop::element::ElementId>
    g_localPlayerElementIdAtomic{coop::element::kInvalidId};
}  // namespace

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
    // Allocation-free walk (RAM-balloon root-cause fix 2026-06-10): the old
    // ClassNameOf/ToString compares constructed TWO strings per UObject
    // scanned (~250k/walk) -- the wstring bomb every "cold rescan in a
    // no-player window" balloon traced back to. Class matched by pointer
    // (primed on the first textual hit); names compared against the
    // per-thread scratch. The primed UClass* persists across rescans
    // (game-thread-only static); a BP class dies on world unload and its
    // address can be recycled, so it is revalidated (live + still named
    // mainPlayer_C) before each walk trusts it.
    static void* sMpClass = nullptr;
    if (sMpClass && (!R::IsLive(sMpClass) ||
                     !R::NameEquals(R::NameOf(sMpClass), P::name::MainPlayerClass))) {
        sMpClass = nullptr;
    }
    void* mpClass = sMpClass;
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
            sMpClass = cls;
        }
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // skip CDO
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
    // Warm-cache validation by IDENTITY, not possession. The local mainPlayer_C
    // never becomes a puppet, so "alive AND not a registered puppet" is the stable
    // invariant for a known-good cached pointer. Do NOT re-require a non-null
    // Controller here: a POSSESSABLE local -- driving the ATV/quadbike or sitting on
    // a kerfur -- hands its Controller to the vehicle pawn (PlayerController.Possess
    // unpossesses mainPlayer_C) yet is still the local player. The old GetController
    // check invalidated this cache EVERY FRAME while seated, so Local() fell through
    // to RescanLocal() -- a full GUObjectArray walk with a std::wstring alloc per
    // UObject (hundreds of k) -- per rendered frame via the unconditional
    // nameplate::Update(), collapsing the host to ~5 fps and ballooning the seated
    // peer's RAM (the same wstring-bomb shape as the 19 GB Install regression). The
    // COLD RescanLocal() still uses GetController as the tie-breaker among the
    // mainPlayer_C instances (local vs puppets); only the re-validation of an
    // already-resolved cache changed. (quadbike RE 2026-06-08.)
    if (localCached_ && R::IsLive(localCached_) && !IsPuppet(localCached_)) {
        return localCached_;
    }
    // Negative-result TTL (v56 menu-window balloon fix, see header): a miss is
    // DETERMINISTIC while no gameplay world is up -- and a dead cached pawn
    // mid-travel misses for the whole load window -- so per-tick callers must
    // not re-walk GUObjectArray at tick rate for it. Worst case the new pawn
    // is detected kLocalMissTtlMs late, against a multi-second world load.
    // InvalidateLocal resets both fields -> an explicit level-change re-walks
    // immediately.
    localCached_ = nullptr;
    const unsigned long long now = ::GetTickCount64();
    if (localMissAtMs_ != 0 && now - localMissAtMs_ < kLocalMissTtlMs) {
        return nullptr;
    }
    localCached_ = RescanLocal();
    localMissAtMs_ = localCached_ ? 0 : now;
    return localCached_;
}

uint8_t Registry::LocalPeerId() const {
    return localPeerId_;
}

void Registry::SetLocalPeerId(uint8_t id) {
    // Reads + (via Drop/EnsurePlayerElement_) mutates playerBySlot_ (T-7,
    // GT-only). Callers: net_pump host self-registration each tick + the
    // AssignPeerSlot handshake handler -- both on the game thread.
    UE_ASSERT_GAME_THREAD("players::Registry::SetLocalPeerId (playerBySlot_)");
    // Idempotent on same id (caller pattern: host calls this every tick).
    if (localPeerId_ == id && id < kMaxPeers && playerBySlot_[id]) return;
    // If the local peer id changes (re-assign after reconnect), drop the
    // old slot's Element shadow first so its destructor frees the ElementId.
    if (localPeerId_ != id && localPeerId_ < kMaxPeers) {
        DropPlayerElement_(localPeerId_);
    }
    localPeerId_ = id;
    // D9-2 (PR-FOUNDATION Tier 2): a CLIENT now knows its peer slot, so
    // activate its exclusive ElementId band BEFORE allocating the local
    // Player Element -- otherwise EnsurePlayerElement_ -> AllocLocalId would
    // mint from the pre-slot band and could collide with another client's
    // pre-slot id on the host relay. Slot 0 (host) is skipped: the host
    // allocates from the host range (AllocHostId), never the peer range.
    if (id != kPeerIdHost && id < kMaxPeers) {
        coop::element::Registry::Get().SetLocalPeerBand(id);
    }
    if (id < kMaxPeers) {
        // Create the LOCAL Player Element (puppet=nullptr; the local IS
        // the local player). Other slots' Player Elements track puppets
        // and are created by RegisterPuppet.
        EnsurePlayerElement_(id, /*puppet=*/nullptr);
    }
}

void Registry::InvalidateLocal() {
    localCached_ = nullptr;
    localMissAtMs_ = 0;  // explicit invalidation re-walks immediately (no miss TTL)
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
    // playerBySlot_ is GT-only-by-convention (T-7); the net-thread-safe read is
    // LocalPlayerElementId()'s atomic, not this raw map walk. Enforce GT here.
    UE_ASSERT_GAME_THREAD("players::Registry::GetPlayerElement (playerBySlot_)");
    if (peerSlot >= kMaxPeers) return nullptr;
    return playerBySlot_[peerSlot].get();
}

coop::element::ElementId Registry::LocalPlayerElementId() const {
    // Lock-free atomic read so callers from non-game threads (notably the
    // GNS net-thread AssignPeerSlot stamp in session_status.cpp) get a
    // well-defined value. The atomic is published from EnsurePlayerElement_
    // / DropPlayerElement_ under the game-thread invariant. See namespace
    // comment at top of file for the audit context.
    return g_localPlayerElementIdAtomic.load(std::memory_order_acquire);
}

bool Registry::EstablishMirrorForSlot(uint8_t peerSlot,
                                       coop::element::ElementId wireEid) {
    // Reads + writes playerBySlot_ directly (T-7, GT-only). Callers: the
    // HandleJoin / HandlePlayerJoined / HandleAssignPeerSlot handshake handlers,
    // all dispatched from event_feed::Update on the game thread.
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
    // Idempotent: if the slot already carries an Element whose id matches
    // wireEid, nothing to do. AssignPeerSlot is sent once but a reconnect
    // edge could legitimately re-fire; treat duplicates as no-ops.
    // v16: prior versions also refreshed Element::m_syncContext here on
    // a context change between handshakes; m_syncContext is gone.
    if (auto* existing = playerBySlot_[peerSlot].get()) {
        if (existing->GetId() == wireEid && existing->IsMirror()) {
            return true;
        }
    }
    // Snapshot the puppet pointer the OLD Element carried (if any) so the
    // new MIRROR Player Element preserves the puppet binding for downstream
    // code (RemotePlayer* lookups via Puppet()). The local-slot mirror call
    // (puppet was always nullptr for the local) preserves nullptr correctly.
    coop::RemotePlayer* puppet = nullptr;
    if (auto* existing = playerBySlot_[peerSlot].get()) {
        puppet = existing->Puppet();
    }
    // Drop the locally-allocated placeholder (returns its eid to the
    // appropriate free stack via ~Element -> Registry::FreeId).
    DropPlayerElement_(peerSlot);
    // Build the mirror element, then register it at wireEid. RegisterMirror
    // pattern per [[feedback-registry-register-mirror-pattern]]: install in
    // the slot map FIRST, then RegisterMirror; on failure drain the slot
    // outside the lock so ~Element early-returns (m_id stayed kInvalidId).
    auto mirror = std::make_unique<coop::element::Player>(peerSlot, puppet);
    coop::element::Player* raw = mirror.get();
    playerBySlot_[peerSlot] = std::move(mirror);
    auto& reg = coop::element::Registry::Get();
    if (!reg.RegisterMirror(wireEid, raw)) {
        UE_LOGW("players::Registry: EstablishMirrorForSlot peerSlot=%u "
                "RegisterMirror(0x%08x) failed -- dropping placeholder",
                static_cast<unsigned>(peerSlot), wireEid);
        // Slot collision or out-of-range. Drain the slot so the failed
        // mirror's destructor sees m_id == kInvalidId and early-returns
        // without touching element::Registry (no double-free).
        playerBySlot_[peerSlot].reset();
        return false;
    }
    // Defensive: if the local slot ever gets mirrored (not done today --
    // EstablishMirrorForSlot is called for remote slots only), publish
    // the mirror's wireEid into the cross-thread atomic snapshot for
    // consistency with EnsurePlayerElement_'s publish.
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
    // Mutates playerBySlot_ AND publishes g_localPlayerElementIdAtomic; the
    // publish is the net-thread-safe READ side, the mutation is GT-only (T-7).
    UE_ASSERT_GAME_THREAD("players::Registry::EnsurePlayerElement_ (playerBySlot_)");
    if (peerSlot >= kMaxPeers) return;
    // Idempotent: if an Element already exists with the same (peerSlot, puppet)
    // signature, no-op. Callers like net_pump's host self-registration loop
    // re-invoke this every tick.
    if (auto* existing = playerBySlot_[peerSlot].get()) {
        if (existing->PeerSlot() == peerSlot && existing->Puppet() == puppet) {
            return;
        }
        // A4 (2026-05-29): mirror Player Elements are wire-authoritative
        // (bound to the SENDER's allocation id). If the AssignPeerSlot /
        // Join handshake established a mirror before RegisterPuppet ran,
        // drop+realloc here would destroy the wire binding and the next
        // ItemActivate/Weather/etc. packet's Registry::Get(senderElementId)
        // would fail to resolve. Bind the puppet pointer in place instead.
        if (existing->IsMirror()) {
            existing->SetPuppet_(puppet);
            UE_LOGI("players::Registry: bound puppet=%p to existing MIRROR "
                    "Player Element eid=0x%08x peerSlot=%u",
                    puppet, existing->GetId(), static_cast<unsigned>(peerSlot));
            return;
        }
        // Mismatch: same slot but different puppet (e.g. local re-allocated
        // as a puppet, or vice versa). Drop the old before re-creating.
        DropPlayerElement_(peerSlot);
    }
    auto el = std::make_unique<coop::element::Player>(peerSlot, puppet);
    // (v14 stamped a per-process-monotonic syncContext byte here for the
    // wire stale-gen defense; v16 PR-FOUNDATION-1b moved that defense to
    // the packet header's senderEpoch -- no per-Element context anymore.)
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
    // Publish the local-slot eid to the cross-thread atomic snapshot so
    // net-thread readers (AssignPeerSlot stamp via LocalPlayerElementId)
    // see it lock-free.
    if (peerSlot == localPeerId_) {
        g_localPlayerElementIdAtomic.store(eid, std::memory_order_release);
    }
    UE_LOGI("players::Registry: allocated Player Element eid=%u for peerSlot=%u "
            "(puppet=%p; local=%s; role=%s)",
            eid, peerSlot, puppet,
            puppet ? "no" : "yes", isHost ? "host" : "client");
}

void Registry::DropPlayerElement_(uint8_t peerSlot) {
    // Mutates playerBySlot_ (and clears g_localPlayerElementIdAtomic); GT-only
    // (T-7). The dtor briefly takes element::Registry::m_mutex -- safe on GT.
    UE_ASSERT_GAME_THREAD("players::Registry::DropPlayerElement_ (playerBySlot_)");
    if (peerSlot >= kMaxPeers) return;
    if (!playerBySlot_[peerSlot]) return;
    const auto eid = playerBySlot_[peerSlot]->GetId();
    // Clear the cross-thread atomic snapshot BEFORE the dtor fires so a
    // concurrent net-thread reader can't observe an eid that's about to
    // be freed. Only the local slot is published; other slots aren't
    // mirrored into the atomic.
    if (peerSlot == localPeerId_) {
        g_localPlayerElementIdAtomic.store(coop::element::kInvalidId,
                                            std::memory_order_release);
    }
    // unique_ptr reset -> destructor -> element::Registry::FreeId.
    // No shared lock with element::Registry; the destructor will acquire
    // element::Registry::m_mutex briefly. Safe here on the game thread.
    playerBySlot_[peerSlot].reset();
    UE_LOGI("players::Registry: released Player Element eid=%u for peerSlot=%u",
            eid, peerSlot);
}

}  // namespace coop::players
