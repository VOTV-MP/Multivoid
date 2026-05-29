// coop/players_registry.cpp -- see header for rationale.

#include "coop/players_registry.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/remote_player.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <string>

namespace {
// A4 audit finding 2026-05-29 (CRITICAL #1) + B1 audit finding (Issue #1):
// the AssignPeerSlot stamp in coop/net/session.cpp::HandleConnStatusChanged
// runs on the GNS net thread, not the game thread. Reading through the non-
// atomic `unique_ptr<Player>` in `playerBySlot_[localPeerId_]` from that
// thread is data-race UB. Original A4 fix used two separate atomics for
// (eid, context); a game-thread DropPlayerElement_ between the
// net-thread's TWO loads would stamp a coherent eid with a stale (cleared)
// context byte. Pack (eid:32, ctx:8) into ONE uint64 atomic so a single
// load returns a coherent pair, eliminating the inter-load race.
// Layout: (uint64_t(eid) << 32) | uint64_t(ctx).
inline constexpr uint64_t kInvalidIdCtxPair =
    (uint64_t(coop::element::kInvalidId) << 32);
std::atomic<uint64_t> g_localPlayerIdentityAtomic{kInvalidIdCtxPair};

inline uint64_t PackIdentity_(coop::element::ElementId eid, uint8_t ctx) {
    return (uint64_t(eid) << 32) | uint64_t(ctx);
}
inline coop::element::ElementId UnpackEid_(uint64_t v) {
    return static_cast<coop::element::ElementId>(v >> 32);
}
inline uint8_t UnpackCtx_(uint64_t v) {
    return static_cast<uint8_t>(v & 0xFFu);
}

// v14 (B1): per-process monotonic generation counter for fresh Player
// Element syncContexts. Each fresh local Player Element gets the
// post-increment value (skipping 0 so 0 unambiguously means "no
// Element yet" on the wire). Persists across Session::Stop/Start in
// the same process so rapid disconnect+reconnect on the same eid gets
// a distinguishable generation byte.
//
// Wrap: low-8-bit cycle is 255 (0 is skipped). After 255 reconnects
// within ONE process lifetime an old context COULD alias the current
// one and a stale in-flight packet from that ancient generation could
// pass the receiver-side compare. Harmless in practice (LAN coop +
// reconnect storms of that magnitude are pathological); not a problem
// to engineer around for v1.
std::atomic<uint32_t> g_localContextGen{0};

uint8_t NextLocalContext_() {
    // Increment-and-take; skip 0 because 0 is the "no Element" sentinel
    // on the wire (paired with senderElementId == 0).
    for (;;) {
        const uint32_t v = g_localContextGen.fetch_add(1,
                                std::memory_order_relaxed) + 1;
        const uint8_t ctx = static_cast<uint8_t>(v & 0xFF);
        if (ctx != 0) return ctx;
    }
}
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

coop::element::ElementId Registry::LocalPlayerElementId() const {
    // Lock-free atomic read so callers from non-game threads (notably the
    // GNS net-thread AssignPeerSlot stamp in session.cpp) get a well-
    // defined value. The atomic is published from EnsurePlayerElement_ /
    // DropPlayerElement_ under the game-thread invariant. See namespace
    // comment at top of file for the audit context. B1 v2: backed by
    // the combined (eid, ctx) uint64 atomic; extracts the eid half.
    return UnpackEid_(
        g_localPlayerIdentityAtomic.load(std::memory_order_acquire));
}

uint8_t Registry::LocalPlayerSyncContext() const {
    // Companion to LocalPlayerElementId; lock-free read of the local
    // Player Element's syncContext byte. Stays 0 when no local Element
    // exists (boot/seed window) -- matches the wire convention that a
    // 0 byte alongside a 0 senderElementId means "no element yet".
    // B1 v2: extracts the ctx half of the combined identity atomic.
    return UnpackCtx_(
        g_localPlayerIdentityAtomic.load(std::memory_order_acquire));
}

void Registry::LocalPlayerIdentity(coop::element::ElementId& outEid,
                                    uint8_t& outCtx) const {
    // Atomic-paired accessor. Use this when both eid AND context are
    // needed at the same instant -- e.g. the AssignPeerSlot
    // stamp on the GNS net thread, where a game-thread DropPlayerElement_
    // between two separate loads could yield an eid/ctx tuple from
    // different generations.
    const uint64_t v =
        g_localPlayerIdentityAtomic.load(std::memory_order_acquire);
    outEid = UnpackEid_(v);
    outCtx = UnpackCtx_(v);
}

bool Registry::EstablishMirrorForSlot(uint8_t peerSlot,
                                       coop::element::ElementId wireEid,
                                       uint8_t wireContext) {
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
    // edge could legitimately re-fire; treat duplicates as no-ops. If the
    // wireContext changed between handshakes (sender reset its Element),
    // update the existing mirror's context in place rather than rebuild.
    if (auto* existing = playerBySlot_[peerSlot].get()) {
        if (existing->GetId() == wireEid && existing->IsMirror()) {
            if (existing->GetSyncContext() != wireContext) {
                existing->SetSyncContext(wireContext);
                // Defensive (matches the publish path below): if this idempotent
                // refresh ever covers the LOCAL slot, republish the new context
                // through the cross-thread atomic so outbound stamps via
                // LocalPlayerSyncContext() carry the refreshed value. Without
                // this, a reconnect-with-fresh-context that hits the idempotent
                // branch (eid matched, ctx differed) leaves the atomic frozen
                // on the old ctx, causing all outbound weather/redsky/etc. to
                // mismatch the receiver's just-updated mirror -> silent drops.
                if (peerSlot == localPeerId_) {
                    g_localPlayerIdentityAtomic.store(
                        PackIdentity_(wireEid, wireContext),
                        std::memory_order_release);
                }
                UE_LOGI("players::Registry: refreshed MIRROR Player Element "
                        "eid=0x%08x peerSlot=%u context=0x%02x",
                        wireEid, static_cast<unsigned>(peerSlot),
                        static_cast<unsigned>(wireContext));
            }
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
    // v14 (B1): stamp the wire-carried context BEFORE RegisterMirror so a
    // subsequent senderContext compare against this mirror succeeds even
    // if a packet arrives between RegisterMirror and the log line below.
    mirror->SetSyncContext(wireContext);
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
    // the mirror's wireEid + context into the cross-thread atomic
    // snapshot for consistency with EnsurePlayerElement_'s publish.
    if (peerSlot == localPeerId_) {
        g_localPlayerIdentityAtomic.store(PackIdentity_(wireEid, wireContext),
                                          std::memory_order_release);
    }
    UE_LOGI("players::Registry: established MIRROR Player Element eid=0x%08x "
            "for peerSlot=%u context=0x%02x (puppet=%p; isHostRange=%s)",
            wireEid, static_cast<unsigned>(peerSlot),
            static_cast<unsigned>(wireContext), puppet,
            coop::element::Registry::IsHostId(wireEid) ? "yes" : "no");
    return true;
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
    // v14 (B1, 2026-05-29): stamp a fresh per-process-monotonic syncContext
    // on the new Element. Receivers compare against this byte after the
    // handshake propagates it; a fresh generation distinguishes a
    // post-reconnect Element from a same-id pre-disconnect Element.
    const uint8_t ctx = NextLocalContext_();
    el->SetSyncContext(ctx);
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
    // Publish the local-slot (eid, ctx) pair to the cross-thread atomic
    // snapshot so net-thread readers (AssignPeerSlot stamp + any
    // senderContext stamper) see them lock-free as a coherent pair.
    if (peerSlot == localPeerId_) {
        g_localPlayerIdentityAtomic.store(PackIdentity_(eid, ctx),
                                          std::memory_order_release);
    }
    UE_LOGI("players::Registry: allocated Player Element eid=%u context=0x%02x "
            "for peerSlot=%u (puppet=%p; local=%s; role=%s)",
            eid, static_cast<unsigned>(ctx), peerSlot, puppet,
            puppet ? "no" : "yes", isHost ? "host" : "client");
}

void Registry::DropPlayerElement_(uint8_t peerSlot) {
    if (peerSlot >= kMaxPeers) return;
    if (!playerBySlot_[peerSlot]) return;
    const auto eid = playerBySlot_[peerSlot]->GetId();
    // Clear the cross-thread atomic snapshot BEFORE the dtor fires so a
    // concurrent net-thread reader can't observe an eid that's about to
    // be freed. Only the local slot is published; other slots aren't
    // mirrored into the atomic. Single-store clears the (eid,ctx) pair
    // atomically.
    if (peerSlot == localPeerId_) {
        g_localPlayerIdentityAtomic.store(kInvalidIdCtxPair,
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
