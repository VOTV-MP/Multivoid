// coop/creatures/piramid_sync.cpp -- see coop/creatures/piramid_sync.h. The pyramid-event
// choreography lane: client-mirror brain suppression and the host-to-client gather relay.
// Everything else about the pyramid rides the generic rails: the world-actor pose mirror, the
// NPC lane for the wisps, the event replay verdict, the native event registry parity.

#include "coop/creatures/piramid_sync.h"

#include "coop/element/element.h"
#include "coop/element/mirror_managers.h"  // WaMirrors() / NpcMirrors()
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/element/world_actor.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::piramid_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

constexpr const wchar_t* kPyramidClassName = L"piramid2_C";
constexpr const char*    kPyramidTypeName  = "piramid2_C";

// The native reached-check commits at a 2D distance of 10000 or less. Both actors freeze at
// the host's commit (gathering latches, and the wisp's gather holds it), so the mirrors
// converge to the host's frozen distance, under the threshold by construction but often only
// just, since the 1 Hz check trips right under it. The pre-gate therefore sits above the
// native radius (it only avoids dispatching while the interpolation is still far out); the
// native re-check inside the replayed call is the real arbiter, and a branch-not-taken attempt
// is retried until the deadline. A pre-gate below the native radius starved the replay: the
// frozen distance landed between the two.
constexpr float kReplayAttemptRadius = 10500.0f;
// A gather relay that cannot converge (the mirror missing, the interpolation never closing) is
// dropped loudly after this window; the wisp's death still arrives through the NPC lane's
// destroy.
constexpr long long kReplayDeadlineMs = 5000;

std::atomic<coop::net::Session*> g_session{nullptr};
inline coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

// Lazy hook arming, one-shot and latched. The pyramid class loads only when the event chain
// spawns it, so resolution is gated on a pyramid world-actor Element existing (host: allocated
// at the deferred spawn; client: materialised from the wire). A member miss on the then-loaded
// class can never heal (name drift on a future game version), so one attempt, latched loudly.
bool g_armed = false;
bool g_armFailedLatched = false;
std::atomic<bool> g_armedAtomic{false};

void* g_fnSeeWisps = nullptr;
void* g_fnCheckIfReached = nullptr;
void* g_fnRandLoc = nullptr;
void* g_fnChangeLook = nullptr;  // the 1 Hz random head-wander re-roll, suppressed on mirrors
int32_t g_offWispTarget = -1;
int32_t g_isWalkingOff = -1;
uint8_t g_isWalkingMask = 0;
int32_t g_gatheringOff = -1;
uint8_t g_gatheringMask = 0;
// The facing axis: the pyramid's visible heading lives in the world rotation of its two arrow
// components (the actor root never yaws; the animation orients the body off the component).
// The host's turning step interpolates both components; the mirror's brain is suppressed, so
// nothing turns them and it would face its spawn heading forever.
int32_t g_offMovementVector = -1;
int32_t g_offArrow = -1;
int32_t g_offRelLook = -1;  // the head axis: the look-at's idle target vector

// The thread-local allow slot for the client gather replay: our own re-dispatch of the
// reached-check must pass the brain-suppress interceptor; thread-local because the whole
// stage-and-call sequence is one inline game-thread block.
thread_local bool t_allowCheckIfReached = false;

// The host gather edge detector: pyramid actor to its last-seen gathering flag (game thread
// only; the reached-check dispatch is timer-fired). Swept at 1 Hz against the live world-actor
// set, so a recycled heap address cannot inherit a stale true and eat a fresh pyramid's first
// relay.
std::unordered_map<void*, bool> g_lastGathering;

// The client's pending gather, a single slot: the host serialises gathers behind its own
// gathering latch, so a newer relay legitimately supersedes an unconverged older one.
struct PendingGather {
    uint32_t pyramidEid = 0;
    uint32_t wispEid = 0;
    long long deadlineMs = 0;
    bool active = false;
    float lastDist = -1.0f;  // last measured mirror 2D dist (deadline diagnostics)
    int attempts = 0;        // dispatch attempts (branch-not-taken retries)
};
PendingGather g_pending;

// Client: pyramid mirror eids whose actor tick we restored. The world-actor sync parks generic
// mirrors tick-off, but the pyramid's tick carries the beam params, the look-at and the hover
// smoothing.
std::unordered_set<uint32_t> g_tickRestored;

// The facing axis streams as the pose snapshot's auxiliary yaw, the host's true component
// heading at pose cadence. Deriving it from position deltas was refuted live: the native
// heading keeps easing toward the walk target for up to 10 s after motion stops, which no
// motion delta can observe.

std::atomic<int> g_relayCount{0};
std::atomic<int> g_replayCount{0};

long long g_lastProbeMs = 0;   // 250 ms pre-arm probe / client restore-scan throttle
long long g_lastSweepMs = 0;   // 1 s host edge-map sweep throttle

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool ReadBoolAt(void* obj, int32_t off, uint8_t mask) {
    return (*(reinterpret_cast<uint8_t*>(obj) + off) & mask) != 0;
}
void WriteBoolAt(void* obj, int32_t off, uint8_t mask, bool v) {
    uint8_t& b = *(reinterpret_cast<uint8_t*>(obj) + off);
    if (v) b |= mask; else b &= static_cast<uint8_t>(~mask);
}

// Brain suppression on client mirrors: a PRE interceptor on the state-writing timer handlers
// (every walk-to caller). Any client-side pyramid is a mirror, since the client scheduler is
// dormant and the world-actor sync suppresses local allowlisted spawns, so a per-role gate is
// the per-self gate for this class.
bool BrainSuppress_Interceptor(void* /*self*/, void* /*params*/) {
    auto* s = LoadSession();
    if (!s || !s->connected()) return false;                 // solo/pre-connect: native runs
    if (s->role() != coop::net::Role::Client) return false;  // host brain is authoritative
    if (t_allowCheckIfReached) return false;                 // our own gather replay dispatch
    return true;  // cancel the mirror's brain step
}

// Host actor to lane identity: linear snapshots over the live elements, called only on a
// gather commit edge, once per consumed wisp.
uint32_t FindWaEidForActor(void* actor) {
    std::vector<coop::element::WorldActor*> snap;
    coop::element::WaMirrors().Snapshot(snap);
    for (auto* el : snap)
        if (el && el->GetActor() == actor) return static_cast<uint32_t>(el->GetId());
    return 0;
}
uint32_t FindNpcEidForActor(void* actor) {
    std::vector<coop::element::Npc*> snap;
    coop::element::NpcMirrors().Snapshot(snap);
    for (auto* el : snap)
        if (el && el->GetActor() == actor) return static_cast<uint32_t>(el->GetId());
    return 0;
}

// The host gather detect: a POST observer on the reached-check (a 1 Hz timer dispatch, game
// thread). The gather commit is inside the function body (walking cleared, the wisp's gather,
// gathering set, the montage), so the POST reads the just-written flag and edge-detects its
// rise. The montage-completed path resets it off this seam; the next dispatch's POST records
// the falling edge (no relay) and re-arms the detector for the next wisp.
void CheckIfReached_POST(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (g_gatheringOff < 0 || g_offWispTarget < 0) return;
    const bool gathering = ReadBoolAt(self, g_gatheringOff, g_gatheringMask);
    bool& last = g_lastGathering[self];
    if (gathering == last) return;
    last = gathering;
    if (!gathering) return;  // falling edge: gather finished -- nothing to relay
    void* wisp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(self) + g_offWispTarget);
    if (!wisp) {
        UE_LOGW("piramid-gather[host]: gathering rose with null wispTarget -- not relaying");
        return;
    }
    const uint32_t pyramidEid = FindWaEidForActor(self);
    const uint32_t wispEid = FindNpcEidForActor(wisp);
    if (pyramidEid == 0 || wispEid == 0) {
        UE_LOGW("piramid-gather[host]: identity unresolved (pyramidEid=%u wispEid=%u) -- not "
                "relaying (unmirrored wisp? WA element raced?)", pyramidEid, wispEid);
        return;
    }
    coop::net::PyramidGatherPayload p{};
    p.pyramidEid = pyramidEid;
    p.wispEid = wispEid;
    if (s->SendReliable(coop::net::ReliableKind::PyramidGather, &p, sizeof(p))) {
        g_relayCount.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("piramid-gather[host]: relayed gather pyramidEid=%u wispEid=%u", pyramidEid, wispEid);
    } else {
        UE_LOGW("piramid-gather[host]: SendReliable(PyramidGather) FAILED (pyramidEid=%u wispEid=%u)",
                pyramidEid, wispEid);
    }
}

// Arming.
bool AnyPyramidElementExists() {
    std::vector<coop::element::WorldActor*> snap;
    coop::element::WaMirrors().Snapshot(snap);
    for (auto* el : snap)
        if (el && el->GetTypeName() == kPyramidTypeName) return true;
    return false;
}

void TryArmHooks() {
    if (g_armed || g_armFailedLatched) return;
    if (!AnyPyramidElementExists()) return;  // class not needed yet -- no GUObjectArray walk
    void* cls = R::FindClass(kPyramidClassName);
    if (!cls) {
        // An element with this type name exists, so the class must be loaded; a miss here is name
        // drift, which retrying can never heal.
        g_armFailedLatched = true;
        UE_LOGE("piramid-brain: FindClass(%ls) FAILED with a piramid element live -- lane "
                "DISABLED for process (pose mirror still rides world_actor_sync)", kPyramidClassName);
        return;
    }
    g_fnSeeWisps = R::FindFunction(cls, L"seeWisps");
    g_fnCheckIfReached = R::FindFunction(cls, L"checkIfReached");
    g_fnRandLoc = R::FindFunction(cls, L"randLoc");
    g_fnChangeLook = R::FindFunction(cls, L"changeLook");
    g_offWispTarget = R::FindPropertyOffset(cls, L"wispTarget");
    // The facing axis, auxiliary: a miss disables only the heading drive, not the lane. The two
    // heading arrow components the host's turning step rotates.
    g_offMovementVector = R::FindPropertyOffset(cls, L"movementVector");
    g_offArrow = R::FindPropertyOffset(cls, L"Arrow");
    if (g_offMovementVector < 0)
        UE_LOGW("piramid-brain: movementVector offset unresolved -- client mirror facing will stay "
                "at spawn heading (name drift?)");
    // The head axis, auxiliary like the facing pair: a miss disables only the head stream.
    g_offRelLook = R::FindPropertyOffset(cls, L"relLook");
    if (g_offRelLook < 0)
        UE_LOGW("piramid-brain: relLook offset unresolved -- the mirror head keeps its own "
                "wander (name drift?)");
    const bool boolsOk =
        R::FindBoolProperty(cls, L"isWalking", g_isWalkingOff, g_isWalkingMask) &&
        R::FindBoolProperty(cls, L"gathering", g_gatheringOff, g_gatheringMask);
    if (!g_fnSeeWisps || !g_fnCheckIfReached || !g_fnRandLoc || !g_fnChangeLook ||
        g_offWispTarget < 0 || !boolsOk) {
        g_armFailedLatched = true;
        UE_LOGE("piramid-brain: member resolve FAILED (seeWisps=%p checkIfReached=%p randLoc=%p "
                "changeLook=%p wispTarget=%d bools=%d) -- lane DISABLED for process (name drift?)",
                g_fnSeeWisps, g_fnCheckIfReached, g_fnRandLoc, g_fnChangeLook, g_offWispTarget,
                boolsOk ? 1 : 0);
        return;
    }
    // Register all or disable: a partial set would half-suppress the mirror brain, worse than
    // none; seeWisps alive with the reached-check dead latches walking forever.
    const bool i1 = GT::RegisterInterceptor(g_fnSeeWisps, &BrainSuppress_Interceptor);
    const bool i2 = i1 && GT::RegisterInterceptor(g_fnCheckIfReached, &BrainSuppress_Interceptor);
    const bool i3 = i2 && GT::RegisterInterceptor(g_fnRandLoc, &BrainSuppress_Interceptor);
    // changeLook joins the suppressed set: its 1 Hz random look re-roll is the head divergence,
    // and the streamed host vector is the one writer on a mirror.
    const bool i4 = i3 && GT::RegisterInterceptor(g_fnChangeLook, &BrainSuppress_Interceptor);
    const bool o1 = i4 && GT::RegisterPostObserver(g_fnCheckIfReached, &CheckIfReached_POST);
    if (!o1) {
        if (i1) GT::UnregisterInterceptor(g_fnSeeWisps, &BrainSuppress_Interceptor);
        if (i2) GT::UnregisterInterceptor(g_fnCheckIfReached, &BrainSuppress_Interceptor);
        if (i3) GT::UnregisterInterceptor(g_fnRandLoc, &BrainSuppress_Interceptor);
        if (i4) GT::UnregisterInterceptor(g_fnChangeLook, &BrainSuppress_Interceptor);
        g_armFailedLatched = true;
        UE_LOGE("piramid-brain: hook table FULL (interceptors %d/%d/%d/%d, observer %d) -- lane "
                "DISABLED for process + rolled back", i1, i2, i3, i4, o1);
        return;
    }
    g_armed = true;
    g_armedAtomic.store(true, std::memory_order_release);
    UE_LOGI("piramid-brain: armed -- 4 brain interceptors (seeWisps/checkIfReached/randLoc/"
            "changeLook) + checkIfReached POST (gather edge); wispTarget@%d isWalking@%d/%02x "
            "gathering@%d/%02x relLook@%d",
            g_offWispTarget, g_isWalkingOff, g_isWalkingMask, g_gatheringOff, g_gatheringMask,
            g_offRelLook);
}

// Client mirror upkeep. A new pyramid mirror: undo anything its brain wrote in the pre-arm
// window (up to 250 ms, in which a seeWisps could have latched walking and a target before the
// interceptors existed), then restore its actor tick (beams, look-at, hover; the march stays
// zero with walking cleared).
void RestoreNewClientMirrors() {
    std::vector<coop::element::WorldActor*> snap;
    coop::element::WaMirrors().Snapshot(snap);
    for (auto* el : snap) {
        if (!el || !el->IsMirror() || el->GetTypeName() != kPyramidTypeName) continue;
        const uint32_t eid = static_cast<uint32_t>(el->GetId());
        if (g_tickRestored.count(eid)) continue;
        void* actor = el->GetActor();
        if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx())) continue;
        WriteBoolAt(actor, g_isWalkingOff, g_isWalkingMask, false);
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + g_offWispTarget) = nullptr;
        E::SetActorTickEnabled(actor, true);
        g_tickRestored.insert(eid);
        UE_LOGI("piramid-brain[client]: mirror eid=%u unstaged + actor tick restored (beam/look-at "
                "drive live; brain suppressed)", eid);
    }
}

// Attempt the queued gather replay; retried every tick until converged or the deadline.
void TryReplayPendingGather() {
    if (!g_pending.active) return;
    if (NowMs() > g_pending.deadlineMs) {
        UE_LOGW("piramid-gather[client]: replay DEADLINE (pyramidEid=%u wispEid=%u lastDist=%.0f "
                "attempts=%d) -- dropped; wisp death still arrives via npc EntityDestroy",
                g_pending.pyramidEid, g_pending.wispEid, g_pending.lastDist, g_pending.attempts);
        g_pending.active = false;
        return;
    }
    coop::element::WorldActor* pel = coop::element::WaMirrors().Get(g_pending.pyramidEid);
    if (!pel || !pel->IsMirror()) return;
    void* pyr = pel->GetActor();
    if (!pyr || !R::IsLiveByIndex(pyr, pel->GetInternalIdx())) return;
    coop::element::Npc* wel = coop::element::NpcMirrors().Get(g_pending.wispEid);
    if (!wel) return;
    void* wisp = wel->GetActor();
    if (!wisp || !R::IsLiveByIndex(wisp, wel->GetInternalIdx())) return;
    if (ReadBoolAt(pyr, g_gatheringOff, g_gatheringMask)) {
        // Already mid-gather (a duplicate relay, or a prior replay landed): nothing to stage.
        g_pending.active = false;
        return;
    }
    const auto pl = E::GetActorLocation(pyr);
    const auto wl = E::GetActorLocation(wisp);
    const float dx = pl.X - wl.X, dy = pl.Y - wl.Y;
    g_pending.lastDist = std::sqrt(dx * dx + dy * dy);
    if (g_pending.lastDist > kReplayAttemptRadius) return;  // interp still converging
    // Stage the native inputs the host had at its own commit, then run the game's own branch. The
    // whole block is one inline game-thread sequence, so the tick cannot interleave: the staged
    // walking flag is only ever observed by the reached-check call below, which resets it in its
    // stop path before any tick could march on it.
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pyr) + g_offWispTarget) = wisp;
    WriteBoolAt(pyr, g_isWalkingOff, g_isWalkingMask, true);
    bool called = false;
    {
        ue_wrap::ParamFrame frame(g_fnCheckIfReached);
        if (frame.valid()) {
            t_allowCheckIfReached = true;
            called = ue_wrap::Call(pyr, frame);
            t_allowCheckIfReached = false;
        }
    }
    const bool gathering = ReadBoolAt(pyr, g_gatheringOff, g_gatheringMask);
    ++g_pending.attempts;
    if (!called || !gathering) {
        // Branch not taken (the distance read over the native radius inside the re-check, or the
        // frame failed): unstage, so the alive tick can never march on a stale latch, and retry
        // next tick; the mirrors are still converging toward the host's frozen truth.
        WriteBoolAt(pyr, g_isWalkingOff, g_isWalkingMask, false);
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pyr) + g_offWispTarget) = nullptr;
        return;
    }
    g_replayCount.fetch_add(1, std::memory_order_relaxed);
    // The suck is the wisp's own tick code: the wisp's gather, which the re-dispatched native
    // branch called on this mirror wisp, latches gathered and no movement and snapshots the mesh
    // start point; the rise itself is the wisp tick's gathered branch, which lerps the mesh toward
    // the pyramid's centre and shrinks it. The NPC mirror parks every non-swarm NPC tick off, so
    // without this the client wisp stays grounded and the pyramid's beams, tied to its centre,
    // stay long. Re-enabling the actor tick is safe: with gathered set the hunt AI is unreachable,
    // movement is off, and every reachable branch is a pure derivation of mirrored state, the same
    // principle as the pyramid mirror's own live tick. The wisp dies seconds later at the
    // montage's notify.
    E::SetActorTickEnabled(wisp, true);
    UE_LOGI("piramid-gather[client]: replay OK -- native gather running on mirror; wisp mirror "
            "tick ENABLED for the gathered rise (pyramidEid=%u wispEid=%u dist=%.0f attempts=%d)",
            g_pending.pyramidEid, g_pending.wispEid, g_pending.lastDist, g_pending.attempts);
    g_pending.active = false;
}

// Host, 1 Hz: drop edge-map entries whose actor is no longer a tracked pyramid (the event
// ended); a recycled address must not inherit a stale gathering flag.
void SweepStaleEdgeEntries() {
    if (g_lastGathering.empty()) return;
    std::vector<coop::element::WorldActor*> snap;
    coop::element::WaMirrors().Snapshot(snap);
    for (auto it = g_lastGathering.begin(); it != g_lastGathering.end();) {
        bool live = false;
        for (auto* el : snap)
            if (el && el->GetActor() == it->first) { live = true; break; }
        if (live) ++it; else it = g_lastGathering.erase(it);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    const long long now = NowMs();

    if (!g_armed) {
        if (g_armFailedLatched) return;
        if (now - g_lastProbeMs < 250) return;  // 250 ms: client must arm well inside the
        g_lastProbeMs = now;                    // BeginPlay+1s first-timer window
        TryArmHooks();
        return;
    }

    if (s->role() == coop::net::Role::Host) {
        if (now - g_lastSweepMs >= 1000) {
            g_lastSweepMs = now;
            SweepStaleEdgeEntries();
        }
        return;
    }

    // Client: the restore scan at 250 ms; the pending replay every tick (cheap lookups, rare).
    if (now - g_lastProbeMs >= 250) {
        g_lastProbeMs = now;
        RestoreNewClientMirrors();
    }
    TryReplayPendingGather();
}

bool ReadHostHeadingYaw(void* actor, float& outYaw) {
    // Pre-arm (the first 250 ms after enrol) or an offset miss: the caller falls back to the actor
    // yaw. No live check here; the caller just checked the actor.
    if (!actor || !g_armedAtomic.load(std::memory_order_acquire) || g_offMovementVector < 0)
        return false;
    void* mv = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + g_offMovementVector);
    if (!mv) return false;
    outYaw = E::GetComponentWorldRotation(mv).Yaw;
    return true;
}

void ApplyMirrorHeadingYaw(void* actor, float yaw) {
    if (!actor || !g_armedAtomic.load(std::memory_order_acquire) || g_offMovementVector < 0)
        return;
    const ue_wrap::FRotator rot{0.f, yaw, 0.f};
    void* mv = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + g_offMovementVector);
    if (mv) E::SetComponentWorldRotation(mv, rot);
    if (g_offArrow >= 0) {
        void* ar = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + g_offArrow);
        if (ar) E::SetComponentWorldRotation(ar, rot);
    }
}

bool ReadHostRelLook(void* actor, float& outX, float& outY, float& outZ) {
    if (!actor || !g_armedAtomic.load(std::memory_order_acquire) || g_offRelLook < 0)
        return false;
    const float* v = reinterpret_cast<const float*>(
        reinterpret_cast<const uint8_t*>(actor) + g_offRelLook);
    outX = v[0]; outY = v[1]; outZ = v[2];
    return true;
}

void ApplyMirrorRelLook(void* actor, float x, float y, float z) {
    if (!actor || !g_armedAtomic.load(std::memory_order_acquire) || g_offRelLook < 0)
        return;
    // All zero means the host stream had no value (pre-arm, or an offset miss on the host side):
    // keep the mirror's current target rather than yanking the head to the origin.
    if (x == 0.f && y == 0.f && z == 0.f) return;
    float* v = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(actor) + g_offRelLook);
    v[0] = x; v[1] = y; v[2] = z;
}

namespace {
// The wisp-target eid cache: the resolve walks the NPC element table (a heap snapshot), fine
// on the rare gather edge but not at the pose stream's rate. The target changes on the 1 Hz
// seeWisps, gather and delete cadence, so a single pointer-to-eid memo collapses the per-tick
// cost to one pointer compare. Game thread only. One entry suffices: the event spawns one
// pyramid (two alternating pyramids would degrade to a per-tick resolve, not break).
void*    g_lastHostWisp = nullptr;
uint32_t g_lastHostWispEid = 0;
}  // namespace

uint32_t ReadHostWispTargetEid(void* actor) {
    // During the walk-to-wisp phase the host's head eases toward the target's world location (the
    // tick's chase branch), while the mirror, its target nulled by design until the gather relay,
    // idled on a look wander the host itself was ignoring. So the target identity is streamed and
    // the mirror runs the same native branch. 0 means no target (pre-arm, a miss, or genuinely
    // idle).
    if (!actor || !g_armedAtomic.load(std::memory_order_acquire) || g_offWispTarget < 0)
        return 0;
    void* wisp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + g_offWispTarget);
    if (!wisp) { g_lastHostWisp = nullptr; g_lastHostWispEid = 0; return 0; }
    if (wisp == g_lastHostWisp && g_lastHostWispEid != 0) return g_lastHostWispEid;
    const uint32_t eid = FindNpcEidForActor(wisp);  // once per target CHANGE (1 Hz seeWisps cadence)
    g_lastHostWisp = wisp;
    g_lastHostWispEid = eid;
    return eid;
}

void ApplyMirrorWispTarget(void* actor, uint32_t wispEid) {
    if (!actor || !g_armedAtomic.load(std::memory_order_acquire) ||
        g_offWispTarget < 0 || g_gatheringOff < 0)
        return;
    // While the mirror is mid-gather the field belongs to the gather choreography (the replay
    // staged it; the montage's notify consumes and clears it natively). The host clears its own
    // target on its own montage clock, and pushing that clear here mid-suck would strand the
    // mirror's delete path. Hands off until the mirror's gathering falls.
    if (ReadBoolAt(actor, g_gatheringOff, g_gatheringMask)) return;
    void** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actor) + g_offWispTarget);
    if (wispEid == 0) {
        if (*slot) {
            *slot = nullptr;
            UE_LOGI("piramid-brain[client]: mirror wispTarget CLEARED (host idle)");
        }
        return;
    }
    coop::element::Npc* wel = coop::element::NpcMirrors().Get(wispEid);
    if (!wel) return;  // wisp mirror not materialized yet -- the next batch retries naturally
    void* wisp = wel->GetActor();
    if (!wisp || !R::IsLiveByIndex(wisp, wel->GetInternalIdx())) return;
    if (*slot != wisp) {
        *slot = wisp;
        UE_LOGI("piramid-brain[client]: mirror wispTarget -> npc eid=%u (native chase branch "
                "steers head/searchlight during the walk)", wispEid);
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!g_armed) return;  // no pyramid ever seen this process -> nothing can be mid-gather
    // Read the current truth off the live actors, not the edge map, which can lag the 1 Hz sweep:
    // a pyramid whose gathering flag is latched right now is mid-choreography, so the commit is
    // re-sent to this joiner and its mirror replays it (the original relay fired before this peer
    // connected). The wisp may already be consumed (the NPC lane retired it); then the joiner only
    // misses the beam tail, and there is nothing valid to stage, so skip.
    std::vector<coop::element::WorldActor*> snap;
    coop::element::WaMirrors().Snapshot(snap);
    for (auto* el : snap) {
        if (!el || el->GetTypeName() != kPyramidTypeName) continue;
        void* pyr = el->GetActor();
        if (!pyr || !R::IsLiveByIndex(pyr, el->GetInternalIdx())) continue;
        if (!ReadBoolAt(pyr, g_gatheringOff, g_gatheringMask)) continue;
        void* wisp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pyr) + g_offWispTarget);
        const uint32_t wispEid = wisp ? FindNpcEidForActor(wisp) : 0;  // ptr compare only -- no deref
        if (wispEid == 0) {
            UE_LOGI("piramid-gather[host]: join-edge slot=%d -- gather in flight but the wisp is "
                    "already retired; not re-sending (joiner misses only the beam tail)", slot);
            continue;
        }
        coop::net::PyramidGatherPayload p{};
        p.pyramidEid = static_cast<uint32_t>(el->GetId());
        p.wispEid = wispEid;
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::PyramidGather, &p, sizeof(p))) {
            UE_LOGI("piramid-gather[host]: join-edge slot=%d re-sent in-flight gather "
                    "pyramidEid=%u wispEid=%u", slot, p.pyramidEid, p.wispEid);
        } else {
            UE_LOGW("piramid-gather[host]: join-edge slot=%d PyramidGather re-send FAILED "
                    "(pyramidEid=%u wispEid=%u)", slot, p.pyramidEid, p.wispEid);
        }
    }
}

void OnDisconnect() {
    g_pending = PendingGather{};
    g_lastGathering.clear();
    g_tickRestored.clear();
    // The host target memo: a recycled actor address in a new session must not alias the old
    // session's eid.
    g_lastHostWisp = nullptr;
    g_lastHostWispEid = 0;
    // The probe counters are per session: a forced re-run in the same process after a reconnect
    // must not see the previous session's relays.
    g_relayCount.store(0, std::memory_order_relaxed);
    g_replayCount.store(0, std::memory_order_relaxed);
    // The hooks stay latched, role- and session-gated inside, the NPC and world-actor observer
    // shape.
}

void OnPyramidGather(const coop::net::PyramidGatherPayload& payload) {
    auto* s = LoadSession();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("piramid-gather: received on host -- dropping (loopback bounce)");
        return;
    }
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.pyramidEid) ||
        !coop::element::Registry::IsAllowedHostAllocatedEid(payload.wispEid)) {
        UE_LOGW("piramid-gather[client]: eid out of host range (pyramid=%u wisp=%u) -- dropping",
                payload.pyramidEid, payload.wispEid);
        return;
    }
    g_pending.pyramidEid = payload.pyramidEid;
    g_pending.wispEid = payload.wispEid;
    g_pending.deadlineMs = NowMs() + kReplayDeadlineMs;
    g_pending.lastDist = -1.0f;
    g_pending.attempts = 0;
    g_pending.active = true;
    UE_LOGI("piramid-gather[client]: queued replay pyramidEid=%u wispEid=%u (deadline %lld ms)",
            payload.pyramidEid, payload.wispEid, kReplayDeadlineMs);
}

bool DebugHooksArmed() { return g_armedAtomic.load(std::memory_order_acquire); }
int  DebugHostRelayCount() { return g_relayCount.load(std::memory_order_relaxed); }
int  DebugClientReplayCount() { return g_replayCount.load(std::memory_order_relaxed); }

}  // namespace coop::piramid_sync
