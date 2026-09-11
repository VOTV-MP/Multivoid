// coop/items/hook_sync.cpp -- the owner half, the mirrors and the brain-park. The handoff at the
// anchor is hook_anchor.cpp; see the header for the authority split both halves share.

#include "coop/items/hook_sync.h"

#include "coop/items/hook_anchor.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include "hook_sync_detail.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::hook_sync {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace H  = ue_wrap::hook;

namespace detail {
namespace {
std::atomic<coop::net::Session*> g_session{nullptr};
std::vector<Owned>               g_owned;
std::unordered_map<Key, Mirror>  g_mirrors;
std::vector<void*>               g_mirrorActors;
}  // namespace

coop::net::Session*              Session()      { return g_session.load(std::memory_order_acquire); }
std::vector<Owned>&              OwnedHooks()   { return g_owned; }
std::unordered_map<Key, Mirror>& Mirrors()      { return g_mirrors; }
std::vector<void*>&              MirrorActors() { return g_mirrorActors; }

void SetSession(coop::net::Session* s) { g_session.store(s, std::memory_order_release); }

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void NoteMirrorActor(void* actor, bool add) {
    if (!actor) return;
    for (size_t i = 0; i < g_mirrorActors.size(); ++i) {
        if (g_mirrorActors[i] != actor) continue;
        if (!add) g_mirrorActors.erase(g_mirrorActors.begin() + static_cast<ptrdiff_t>(i));
        return;  // already present when adding: the index is a set
    }
    if (add) g_mirrorActors.push_back(actor);
}

void AttachTailToSlotBody(void* mirror, uint8_t slot) {
    auto& reg = coop::players::Registry::Get();
    void* body = nullptr;
    if (slot == reg.LocalPeerId()) {
        body = reg.Local();
    } else if (coop::RemotePlayer* rp = reg.Puppet(slot)) {
        body = rp->GetActor();
    }
    // A null body is not a failure: the puppet may not have spawned yet. The tail simply stays on
    // the mirror's own root until the next state message re-tries, which costs one frame of a cable
    // anchored at the wrong end and never a wrong write.
    if (body) H::AttachTailTo(mirror, body);
}

}  // namespace detail

using namespace detail;

namespace {

// The driver's own beat. The POLL is per-hook and slower than this -- see kPollFlyingMs below.
constexpr uint64_t kDriverMs    = 50;
// Reading a hook's head is two ProcessEvent dispatches, so the poll is gated and the gate's clock
// bumps whether or not anything was sent. atv_sync carries a comment about what happens when it
// does not: bumped only on a send, a quiet entity's clock stays stale and the gate then runs its
// dispatches at the pump rate. 20 Hz in flight -- about a second, and the only phase where the head
// moves fast -- and 4 Hz otherwise, which is owner_entity_sync's own cadence.
constexpr uint64_t kPollFlyingMs = 50;
constexpr uint64_t kPollRestMs   = 250;
// The re-announce IS the late-joiner delivery, so it has to be shorter than the thing it delivers.
// owner_entity_sync uses 10 s for an entity that lives for minutes; a hook's flight is about a
// second, so a hook could be born and die inside one of those gaps.
constexpr uint64_t kKeepaliveMs  = 2000;
// A player has one activeHook, but a committed hook stays in the table until the host answers, and
// they can fire again immediately. The cap is a bound on a wire-driven vector, not a design limit.
constexpr size_t   kMaxOwned     = 8;

constexpr float kMoveEpsUU  = 2.0f;
constexpr float kAngEpsDeg  = 1.0f;
constexpr float kDistEps    = 1.0f;

uint16_t g_nextSeq = 0;
bool     g_parkInstalled = false;
bool     g_parkRefused   = false;   // the interceptor table refused: NO mirrors, for the session
uint64_t g_lastDriverMs  = 0;

bool Finite3(float a, float b, float c) {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c);
}
bool InWorld(float a, float b, float c) {
    return std::fabs(a) <= coop::net::kMaxCoord && std::fabs(b) <= coop::net::kMaxCoord &&
           std::fabs(c) <= coop::net::kMaxCoord;
}

uint8_t FlagsOf(const H::State& s) {
    uint8_t f = 0;
    if (s.thrown)       f |= coop::net::HookFlag_Thrown;
    if (s.attachedA)    f |= coop::net::HookFlag_AttachedA;
    if (s.playerHooked) f |= coop::net::HookFlag_PlayerHooked;
    return f;
}

// ---- the brain-park -----------------------------------------------------------------------------
//
// Cancels the Blueprint tick body for OUR mirrors only. Every peer holds its own real hooks and
// other peers' mirrors in the same class at the same time, so the gate is per-ACTOR, never
// per-role. What one frame of that body does to a viewer is write their own movement velocity
// toward someone else's hook and kick them into a fall.
bool OnHookReceiveTickPre(void* self, void* /*params*/) {
    for (void* a : MirrorActors())
        if (a == self) return true;   // cancel
    return false;
}

void SendState(const Owned& o, const H::State& st) {
    auto* s = Session();
    if (!s) return;
    coop::net::HookStatePayload p{};
    p.seq     = o.seq;
    p.classId = static_cast<uint8_t>(o.kind);
    p.flags   = FlagsOf(st);
    p.ax = st.aLoc.X; p.ay = st.aLoc.Y; p.az = st.aLoc.Z;
    p.aPitch = st.aRot.Pitch; p.aYaw = st.aRot.Yaw; p.aRoll = st.aRot.Roll;
    p.dist = st.dist;
    s->SendReliable(coop::net::ReliableKind::HookState, &p, sizeof(p));
}

void SendDestroy(uint16_t seq) {
    auto* s = Session();
    if (!s) return;
    coop::net::HookDestroyPayload d{};
    d.seq = seq;
    s->SendReliable(coop::net::ReliableKind::HookDestroy, &d, sizeof(d));
}

bool Changed(const H::State& a, const H::State& b) {
    if (FlagsOf(a) != FlagsOf(b)) return true;
    if (std::fabs(a.dist - b.dist) > kDistEps) return true;
    if (std::fabs(a.aLoc.X - b.aLoc.X) > kMoveEpsUU) return true;
    if (std::fabs(a.aLoc.Y - b.aLoc.Y) > kMoveEpsUU) return true;
    if (std::fabs(a.aLoc.Z - b.aLoc.Z) > kMoveEpsUU) return true;
    if (std::fabs(a.aRot.Pitch - b.aRot.Pitch) > kAngEpsDeg) return true;
    if (std::fabs(a.aRot.Yaw   - b.aRot.Yaw)   > kAngEpsDeg) return true;
    if (std::fabs(a.aRot.Roll  - b.aRot.Roll)  > kAngEpsDeg) return true;
    return false;
}

void TickOwner(uint64_t now) {
    auto& reg = coop::players::Registry::Get();
    void* local = reg.Local();
    if (!local) return;

    // The field is left DANGLING by two real paths: prop_hook_C returns without clearing it when
    // the hook is already invalid, and attach_a/attach_b destroy the hook on their reject set
    // without telling the player. Liveness first, always.
    void* ah = H::ActiveHookOf(local);
    if (ah && !R::IsLive(ah)) ah = nullptr;

    if (ah && H::KindOf(ah) != H::Kind::Count) {
        bool known = false;
        for (const auto& o : OwnedHooks())
            if (o.ref.Raw() == ah) { known = true; break; }
        if (!known) {
            if (OwnedHooks().size() >= kMaxOwned) {
                UE_LOGW("hook_sync: %zu owned hooks already tracked -- not taking another",
                        OwnedHooks().size());
            } else {
                Owned o;
                o.ref.Set(ah);
                o.seq  = ++g_nextSeq ? g_nextSeq : ++g_nextSeq;  // seq 0 is the destroy wildcard
                o.kind = H::KindOf(ah);
                OwnedHooks().push_back(o);
                UE_LOGI("hook_sync: OWN seq=%u kind=%u actor=%p -- tracking",
                        o.seq, (unsigned)o.kind, ah);
            }
        }
    }

    for (size_t i = 0; i < OwnedHooks().size();) {
        Owned& o = OwnedHooks()[i];
        void* a = o.ref.Get();
        if (!a) {
            SendDestroy(o.seq);
            UE_LOGI("hook_sync: OWN seq=%u died -- destroy announced", o.seq);
            OwnedHooks().erase(OwnedHooks().begin() + static_cast<ptrdiff_t>(i));
            continue;
        }
        if (o.committed) { ++i; continue; }  // handed to the host; it answers with HookAnchored

        const uint64_t period = o.last.thrown ? kPollFlyingMs : kPollRestMs;
        if (now - o.lastPollMs < period) { ++i; continue; }
        o.lastPollMs = now;   // unconditional, whether or not anything is sent

        H::State st{};
        if (!H::ReadState(a, st)) { ++i; continue; }

        // The game let go of it. Anchored (both ends bitten) is the handoff; anything else is a
        // hook that is about to die and the liveness check above will catch it.
        if (a != ah && st.attachedA && st.attachedB) {
            if (coop::hook_anchor::SendCommit(o.seq, o.kind, a, st)) {
                o.committed = true;
                UE_LOGI("hook_sync: OWN seq=%u anchored -- commit sent, waiting for the host", o.seq);
            }
            ++i;
            continue;
        }

        if (Changed(st, o.last) || now - o.lastSendMs >= kKeepaliveMs) {
            SendState(o, st);
            o.last       = st;
            o.lastSendMs = now;
        }
        ++i;
    }
}

void PruneMirrors() {
    for (auto it = Mirrors().begin(); it != Mirrors().end();) {
        if (it->second.ref.Alive()) { ++it; continue; }
        NoteMirrorActor(it->second.ref.Raw(), false);
        it = Mirrors().erase(it);
    }
}

}  // namespace

namespace detail {

void* InstallMirror(Key key, H::Kind kind, const ue_wrap::FVector& aLoc,
                    const ue_wrap::FRotator& aRot, bool anchored) {
    // NO PARK, NO MIRRORS. The interceptor facility's own header says a guard that cannot register
    // fails in the direction of a mirror the authority still owns -- which here is a hook that
    // reels the viewing peer's own player. An invisible hook is the state before this lane existed;
    // a ticking one is worse than that.
    if (g_parkRefused || !g_parkInstalled) return nullptr;

    DropMirror(key);
    void* m = H::SpawnMirror(kind, aLoc, aRot);
    if (!m) return nullptr;
    Mirror row;
    row.ref.Set(m);
    row.kind     = kind;
    row.anchored = anchored;
    Mirrors()[key] = row;
    NoteMirrorActor(m, true);
    return m;
}

void DropMirror(Key key) {
    auto it = Mirrors().find(key);
    if (it == Mirrors().end()) return;
    void* a = it->second.ref.Get();
    NoteMirrorActor(it->second.ref.Raw(), false);
    Mirrors().erase(it);
    if (a) E::DestroyActor(a);
}

}  // namespace detail

void Install(coop::net::Session* session) {
    if (!GT::IsGameThread()) return;
    detail::SetSession(session);
    coop::hook_anchor::Install(session);
    if (g_parkInstalled || g_parkRefused) return;
    if (!H::EnsureResolved()) return;  // retried from Tick while the class loads
    void* tickFn = H::ReceiveTickFunction();
    if (!tickFn) return;
    if (!GT::RegisterInterceptor(tickFn, &OnHookReceiveTickPre)) {
        g_parkRefused = true;
        UE_LOGE("hook_sync: RegisterInterceptor(hook_C::ReceiveTick) FAILED -- the interceptor "
                "table is full. The lane will create NO mirrors this session: an unparked mirror "
                "reels the viewing peer's own player and kicks them into a fall, which is worse "
                "than a hook nobody else can see.");
        return;
    }
    g_parkInstalled = true;
    UE_LOGI("hook_sync: installed -- hook_C::ReceiveTick PRE-interceptor (per-actor mirror gate)");
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = detail::Session();
    if (!s || !s->connected()) return;
    const uint64_t now = detail::NowMs();
    if (now - g_lastDriverMs < kDriverMs) return;
    g_lastDriverMs = now;

    if (!g_parkInstalled && !g_parkRefused) Install(s);
    if (!H::EnsureResolved()) return;

    TickOwner(now);
    PruneMirrors();
    coop::hook_anchor::TickHost(now);
}

void OnStateMsg(const coop::net::HookStatePayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    if (senderPeerSlot < 0 || senderPeerSlot >= coop::players::kMaxPeers) return;
    if (p.seq == 0) return;                                   // reserved: the destroy wildcard
    if (p.classId >= static_cast<uint8_t>(H::Kind::Count)) return;
    if (!Finite3(p.ax, p.ay, p.az) || !InWorld(p.ax, p.ay, p.az)) return;
    if (!Finite3(p.aPitch, p.aYaw, p.aRoll)) return;
    if (!std::isfinite(p.dist) || p.dist < 0.f || p.dist > coop::net::kMaxCoord) return;
    if (senderPeerSlot == coop::players::Registry::Get().LocalPeerId()) return;  // never our own
    if (!H::EnsureResolved()) return;

    const Key key = MakeKey(static_cast<uint8_t>(senderPeerSlot), p.seq);
    const ue_wrap::FVector  loc{p.ax, p.ay, p.az};
    const ue_wrap::FRotator rot{p.aPitch, p.aYaw, p.aRoll};

    auto it = Mirrors().find(key);
    if (it == Mirrors().end() || !it->second.ref.Alive()) {
        void* m = InstallMirror(key, static_cast<H::Kind>(p.classId), loc, rot, /*anchored=*/false);
        if (!m) return;
        AttachTailToSlotBody(m, static_cast<uint8_t>(senderPeerSlot));
        H::DriveMirror(m, loc, rot, p.dist);
        return;
    }
    if (void* m = it->second.ref.Get()) H::DriveMirror(m, loc, rot, p.dist);
}

void OnDestroyMsg(const coop::net::HookDestroyPayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    // originSlot non-zero only when the HOST speaks for a leaver; otherwise the transport sender.
    const uint8_t slot = p.originSlot ? p.originSlot : static_cast<uint8_t>(senderPeerSlot);
    if (slot >= coop::players::kMaxPeers) return;

    if (p.seq != 0) {
        DropMirror(MakeKey(slot, p.seq));
        return;
    }
    // Wildcard: every hook of that slot, ANCHORED ONES INCLUDED, because this form is only ever
    // sent for a slot that is going away entirely.
    for (auto it = Mirrors().begin(); it != Mirrors().end();) {
        if (SlotOf(it->first) != slot) { ++it; continue; }
        void* a = it->second.ref.Get();
        NoteMirrorActor(it->second.ref.Raw(), false);
        it = Mirrors().erase(it);
        if (a) E::DestroyActor(a);
    }
}

void OnPeerLeftSlot(int slot) {
    if (!GT::IsGameThread() || slot < 0 || slot >= coop::players::kMaxPeers) return;
    size_t dropped = 0;
    for (auto it = Mirrors().begin(); it != Mirrors().end();) {
        // Anchored rows belong to the HOST now. A peer leaving has nothing to do with them, and
        // dropping them here would delete a hook that is in the host's save.
        if (SlotOf(it->first) != static_cast<uint8_t>(slot) || it->second.anchored) { ++it; continue; }
        void* a = it->second.ref.Get();
        NoteMirrorActor(it->second.ref.Raw(), false);
        it = Mirrors().erase(it);
        if (a) E::DestroyActor(a);
        ++dropped;
    }
    if (dropped) UE_LOGI("hook_sync: slot %d left -- %zu owner-phase mirrors dropped", slot, dropped);
}

void OnDisconnect() {
    if (!GT::IsGameThread()) {
        detail::SetSession(nullptr);
        return;
    }
    for (auto& kv : Mirrors()) {
        if (void* a = kv.second.ref.Get()) E::DestroyActor(a);
    }
    Mirrors().clear();
    MirrorActors().clear();
    OwnedHooks().clear();
    coop::hook_anchor::OnDisconnect();
    detail::SetSession(nullptr);
    // g_parkInstalled stays true: the interceptor is process-lifetime and self-restoring, because
    // an empty mirror table makes it answer false for every actor.
}

}  // namespace coop::hook_sync
