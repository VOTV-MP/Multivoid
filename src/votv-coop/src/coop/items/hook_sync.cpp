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

bool AttachTailToSlotBody(void* mirror, uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = rp ? rp->GetActor() : nullptr;
    if (!body) return false;
    return H::AttachTailTo(mirror, body);
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
// A player has one activeHook, but a committed hook stays in the table until the host answers and
// they can fire again immediately, so the local table needs a bound too.
constexpr size_t   kMaxOwned     = 8;
// THE WIRE-DRIVEN BOUND, and the one that matters. Mirrors() takes a new (slot, seq) from any peer
// and spawns a real actor for it, so without a per-sender cap one peer walking `seq` freezes every
// other peer -- and HookState is relayed, so it reaches them all. The owner-entity lane closed
// exactly this hole on its own receive path; this lane copied its send-side cap and needed both.
constexpr size_t   kMaxMirrorsPerSlot = 16;

constexpr float kMoveEpsUU  = 2.0f;
constexpr float kAngEpsDeg  = 1.0f;
constexpr float kDistEps    = 1.0f;

uint16_t g_nextSeq = 0;
bool     g_parkInstalled = false;
bool     g_parkRefused   = false;   // the interceptor table refused: NO mirrors, for the session
uint64_t g_lastDriverMs  = 0;
bool     g_saidOwnedFull = false;   // one line, not one per driver tick
uint8_t  g_saidSlotFull  = 0;       // one line per slot, bit-per-slot
std::atomic<uint32_t> g_tickOffThread{0};  // tripwire: see OnHookReceiveTickPre

bool Finite3(float a, float b, float c) {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c);
}
bool InWorld(float a, float b, float c) {
    return std::fabs(a) <= coop::net::kMaxCoord && std::fabs(b) <= coop::net::kMaxCoord &&
           std::fabs(c) <= coop::net::kMaxCoord;
}

// ---- the brain-park -----------------------------------------------------------------------------
//
// Cancels the Blueprint tick body for OUR mirrors only. Every peer holds its own real hooks and
// other peers' mirrors in the same class at the same time, so the gate is per-ACTOR, never
// per-role. What one frame of that body does to a viewer is write their own movement velocity
// toward someone else's hook and kick them into a fall.
//
// THE THREAD ARGUMENT, stated rather than assumed. An interceptor fires on the dispatching thread,
// which is usually the game thread and sometimes a task-graph worker. ReceiveTick is an ACTOR tick,
// which the engine drives from the game thread's tick groups, so the scan below is single-threaded
// against the game-thread-only writers in NoteMirrorActor. That is an argument, not a proof, so an
// off-thread dispatch is COUNTED and reported rather than assumed away -- and the callback answers
// "do not cancel" there, because the table it would have to read is not safe to read.
bool OnHookReceiveTickPre(void* self, void* /*params*/) {
    if (!GT::IsGameThread()) {
        const uint32_t n = g_tickOffThread.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n % 1000) == 0) {
            UE_LOGW("hook_sync: hook_C::ReceiveTick dispatched OFF the game thread (#%u) -- the "
                    "mirror table cannot be read safely there, so the body was NOT cancelled. If "
                    "this line exists at all, the park's thread argument is wrong.", n);
        }
        return false;
    }
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

// The phase never leaves this machine: it decides WHEN the owner sends, and a display mirror
// renders the same whatever phase it is in.
uint8_t PhaseOf(const H::State& s) {
    return static_cast<uint8_t>((s.thrown ? 1 : 0) | (s.attachedA ? 2 : 0) |
                                (s.playerHooked ? 4 : 0));
}

bool Changed(const H::State& a, const H::State& b) {
    if (PhaseOf(a) != PhaseOf(b)) return true;
    if (std::fabs(a.dist - b.dist) > kDistEps) return true;
    if (std::fabs(a.aLoc.X - b.aLoc.X) > kMoveEpsUU) return true;
    if (std::fabs(a.aLoc.Y - b.aLoc.Y) > kMoveEpsUU) return true;
    if (std::fabs(a.aLoc.Z - b.aLoc.Z) > kMoveEpsUU) return true;
    if (std::fabs(a.aRot.Pitch - b.aRot.Pitch) > kAngEpsDeg) return true;
    if (std::fabs(a.aRot.Yaw   - b.aRot.Yaw)   > kAngEpsDeg) return true;
    if (std::fabs(a.aRot.Roll  - b.aRot.Roll)  > kAngEpsDeg) return true;
    return false;
}

bool AlreadyOwned(void* actor) {
    for (const auto& o : OwnedHooks())
        if (o.ref.Raw() == actor) return true;
    return false;
}

void TickOwner(uint64_t now) {
    void* local = coop::players::Registry::Get().Local();
    if (!local) return;

    // The field is left DANGLING by two real paths: prop_hook_C returns without clearing it when
    // the hook is already invalid, and attach_a/attach_b destroy the hook on their reject set
    // without telling the player.
    //
    // The liveness question is asked in the order that keeps it both cheap and safe. A pointer we
    // already track is answered by its own reference, which never dereferences. Only a pointer we
    // have never seen reaches the bare liveness probe, and that probe DOES dereference -- which is
    // why it must not run on every driver tick against a field that stays dangling until the
    // player next fires.
    void* ah = H::ActiveHookOf(local);
    const bool known = ah && AlreadyOwned(ah);
    if (ah && !known && !R::IsLive(ah)) ah = nullptr;

    if (ah && !known && H::KindOf(ah) != H::Kind::Count) {
        if (OwnedHooks().size() >= kMaxOwned) {
            if (!g_saidOwnedFull) {
                g_saidOwnedFull = true;
                UE_LOGW("hook_sync: %zu owned hooks already tracked -- not taking another. Said "
                        "once; the condition persists while the player holds an untracked hook.",
                        OwnedHooks().size());
            }
        } else {
            Owned o;
            o.ref.Set(ah);
            o.seq  = ++g_nextSeq ? g_nextSeq : ++g_nextSeq;  // seq 0 is not a legal identity
            o.kind = H::KindOf(ah);
            OwnedHooks().push_back(o);
            g_saidOwnedFull = false;
            UE_LOGI("hook_sync: OWN seq=%u kind=%u actor=%p -- tracking",
                    o.seq, (unsigned)o.kind, ah);
        }
    }

    for (size_t i = 0; i < OwnedHooks().size();) {
        Owned& o = OwnedHooks()[i];
        void* a = o.ref.Get();
        if (!a) {
            SendDestroy(o.seq);
            UE_LOGI("hook_sync: OWN seq=%u died -- destroy announced", o.seq);
            OwnedHooks().erase(OwnedHooks().begin() + static_cast<ptrdiff_t>(i));
            g_saidOwnedFull = false;
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
            const coop::hook_anchor::CommitResult r =
                coop::hook_anchor::SendCommit(o.seq, o.kind, a);
            if (r == coop::hook_anchor::CommitResult::AdoptedLocally) {
                // The host IS the arbiter, so there is no round trip: the row moves to the adopted
                // table in the same breath instead of waiting for an answer a host never sends
                // itself. Without this a host-fired hook never hands over at all.
                OwnedHooks().erase(OwnedHooks().begin() + static_cast<ptrdiff_t>(i));
                g_saidOwnedFull = false;
                continue;
            }
            if (r == coop::hook_anchor::CommitResult::Sent) {
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

size_t MirrorsForSlot(uint8_t slot) {
    size_t n = 0;
    for (const auto& kv : Mirrors())
        if (SlotOf(kv.first) == slot) ++n;
    return n;
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

bool DropOwnedBySeq(uint16_t seq) {
    for (size_t i = 0; i < OwnedHooks().size(); ++i) {
        if (OwnedHooks()[i].seq != seq) continue;
        if (void* a = OwnedHooks()[i].ref.Get()) E::DestroyActor(a);
        OwnedHooks().erase(OwnedHooks().begin() + static_cast<ptrdiff_t>(i));
        g_saidOwnedFull = false;
        return true;
    }
    return false;
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
    if (s->role() == coop::net::Role::Host) detail::TickPropClaims();
}

void OnStateMsg(const coop::net::HookStatePayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    // A posted receiver can drain AFTER the teardown has run, and an actor spawned then lingers
    // into single-player -- the one thing the teardown exists to prevent.
    auto* s = detail::Session();
    if (!s || !s->connected()) return;
    if (senderPeerSlot < 0 || senderPeerSlot >= coop::players::kMaxPeers) return;
    if (p.seq == 0) return;                                   // not a legal identity
    if (p.classId >= static_cast<uint8_t>(H::Kind::Count)) return;
    if (!Finite3(p.ax, p.ay, p.az) || !InWorld(p.ax, p.ay, p.az)) return;
    if (!Finite3(p.aPitch, p.aYaw, p.aRoll)) return;
    if (!std::isfinite(p.dist) || p.dist < 0.f || p.dist > coop::net::kMaxCoord) return;
    if (senderPeerSlot == coop::players::Registry::Get().LocalPeerId()) return;  // never our own
    if (!H::EnsureResolved()) return;

    const uint8_t slot = static_cast<uint8_t>(senderPeerSlot);
    // The owner-phase space only. An anchored hook is the host's, and a former owner's stale or
    // forged state must not drive or replace it.
    const Key key = MakeKey(slot, p.seq, /*anchored=*/false);
    const ue_wrap::FVector  loc{p.ax, p.ay, p.az};
    const ue_wrap::FRotator rot{p.aPitch, p.aYaw, p.aRoll};

    auto it = Mirrors().find(key);
    if (it == Mirrors().end() || !it->second.ref.Alive()) {
        if (MirrorsForSlot(slot) >= kMaxMirrorsPerSlot) {
            if (!(g_saidSlotFull & (1u << (slot & 7u)))) {
                g_saidSlotFull |= static_cast<uint8_t>(1u << (slot & 7u));
                UE_LOGW("hook_sync: slot %u is at the %zu-mirror ceiling -- refusing to spawn "
                        "another. One peer must not be able to walk its sequence and make every "
                        "other peer spawn actors.", (unsigned)slot, kMaxMirrorsPerSlot);
            }
            return;
        }
        void* m = InstallMirror(key, static_cast<H::Kind>(p.classId), loc, rot, /*anchored=*/false);
        if (!m) return;
        H::DriveMirror(m, loc, rot, p.dist);
        auto row = Mirrors().find(key);
        if (row != Mirrors().end()) row->second.tailAttached = AttachTailToSlotBody(m, slot);
        return;
    }
    void* m = it->second.ref.Get();
    if (!m) return;
    H::DriveMirror(m, loc, rot, p.dist);
    // The retry an earlier draft only claimed: a puppet that had not spawned when the mirror was
    // built leaves the cable's far end on the mirror's own root, and the update branch drove the
    // head and never looked again.
    if (!it->second.tailAttached) it->second.tailAttached = AttachTailToSlotBody(m, slot);
}

void OnDestroyMsg(const coop::net::HookDestroyPayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    if (senderPeerSlot < 0 || senderPeerSlot >= coop::players::kMaxPeers) return;
    if (p.seq == 0) return;

    // Only the HOST (transport slot 0) may speak for another slot. Without the second half of this
    // term any peer could name any other and delete its hooks on every machine -- the sibling
    // owner-entity lane carries the same guard for the same reason, and this lane lost half of it
    // on the way across.
    uint8_t slot = static_cast<uint8_t>(senderPeerSlot);
    if (p.originSlot != 0 && senderPeerSlot == 0) slot = p.originSlot;
    if (slot >= coop::players::kMaxPeers) return;

    const bool anchored = p.anchored != 0;
    if (anchored && senderPeerSlot != 0) return;  // an anchored hook is the host's to retire

    DropMirror(MakeKey(slot, p.seq, anchored));

    // And if the hook it names is OURS, the local actor goes with it. This is what makes the
    // host's refusal of a commit mean something: without it the owner keeps a real hook that every
    // other peer has dropped, which is precisely the ghost the refusal is meant to prevent.
    if (slot == coop::players::Registry::Get().LocalPeerId() && !anchored) {
        if (DropOwnedBySeq(p.seq)) {
            UE_LOGW("hook_sync: the host retired our own hook seq=%u -- the local actor went with "
                    "it, so nobody is left holding a hook the others cannot see", (unsigned)p.seq);
        }
    }
}

void OnPeerLeftSlot(int slot) {
    if (!GT::IsGameThread() || slot < 0 || slot >= coop::players::kMaxPeers) return;
    size_t dropped = 0;
    for (auto it = Mirrors().begin(); it != Mirrors().end();) {
        // Anchored rows belong to the HOST now. A peer leaving has nothing to do with them, and
        // dropping them here would delete a hook that is in the host's save. They live in their own
        // key space, so the slot this peer frees cannot collide with them either.
        if (SlotOf(it->first) != static_cast<uint8_t>(slot) || IsAnchored(it->first)) { ++it; continue; }
        void* a = it->second.ref.Get();
        NoteMirrorActor(it->second.ref.Raw(), false);
        it = Mirrors().erase(it);
        if (a) E::DestroyActor(a);
        ++dropped;
    }
    g_saidSlotFull = static_cast<uint8_t>(g_saidSlotFull & ~(1u << (slot & 7u)));
    coop::hook_anchor::OnPeerLeftSlot(slot);
    if (dropped) UE_LOGI("hook_sync: slot %d left -- %zu owner-phase mirrors dropped", slot, dropped);
}

void OnDisconnect() {
    // The containers are cleared whatever thread this is. Leaving them loaded carries a dead
    // session's rows into the next one, and a half-cleared state is worse than either end of it.
    const bool gt = GT::IsGameThread();
    if (gt) {
        for (auto& kv : Mirrors())
            if (void* a = kv.second.ref.Get()) E::DestroyActor(a);
    } else {
        UE_LOGW("hook_sync: OnDisconnect off the game thread -- the rows are cleared, but the "
                "mirror actors cannot be destroyed from here and will linger");
    }
    Mirrors().clear();
    MirrorActors().clear();
    OwnedHooks().clear();
    detail::ResetPropClaims();
    g_saidOwnedFull = false;
    g_saidSlotFull  = 0;

    // THE PARK AND THE WRAPPER GO BACK TOO. A Blueprint class dies on world unload and its address
    // can be recycled, so a registration left against one would have the table judging whatever
    // takes that address next -- and worse, the installed flag would still be true, so Install
    // could not re-register for the next world while InstallMirror's refusal guard went on passing.
    // The sibling device lanes return their interceptors for exactly this reason.
    if (gt && g_parkInstalled) {
        if (void* tickFn = H::ReceiveTickFunction())
            GT::UnregisterInterceptor(tickFn, &OnHookReceiveTickPre);
    }
    g_parkInstalled = false;
    g_parkRefused   = false;

    coop::hook_anchor::OnDisconnect();
    H::ResetCache();
    detail::SetSession(nullptr);
}

}  // namespace coop::hook_sync
