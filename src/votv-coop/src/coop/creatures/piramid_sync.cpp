// coop/piramid_sync.cpp -- see coop/creatures/piramid_sync.h.
//
// The pyramid-event choreography lane: client-mirror brain suppression + the host->client
// PyramidGather relay. Everything else about the pyramid rides the generic rails
// (world_actor_sync pose mirror, npc lane wisps, event_fire_sync no-replay verdict, native
// setEvent registry parity). Ground truth for every offset/branch cited below:
// research/findings/props-lifecycle/votv-piramid2-RE-2026-07-04.md.

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

// Native checkIfReached arrives at 2D dist <= 10000 (RE @6323). Both actors FREEZE at the
// host's commit (gathering latches + the wisp's gather() hold), so the mirrors converge to
// the host's frozen distance -- which is <= 10000 BY CONSTRUCTION but often only just (the
// 1 Hz check trips right under the threshold). The pre-gate must therefore sit ABOVE the
// native radius (it only avoids dispatching while interp is still far out); the native
// re-check inside the replayed call is the real arbiter, and a branch-not-taken attempt is
// RETRIED until the deadline (poses keep converging). A 9000 pre-gate here was the proven
// failure mode: frozen dist landed in (9000, 10000] and the replay starved to deadline.
constexpr float kReplayAttemptRadius = 10500.0f;
// A gather relay that cannot converge (mirror missing / interp never closes) is dropped
// LOUD after this window; the wisp's death still arrives via the npc lane's EntityDestroy.
constexpr long long kReplayDeadlineMs = 5000;

std::atomic<coop::net::Session*> g_session{nullptr};
inline coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

// ---- lazy hook arming (one-shot, latched) ----------------------------------------------------
// piramid2_C loads only when the event chain spawns it, so resolution is gated on a piramid
// WorldActor ELEMENT existing (host: interceptor-allocated at BeginDeferred; client: wire-
// materialized) -- the world_actor_sync lazy-class reasoning. A member miss on the then-loaded
// class can never heal (name drift on a future game version) -> single attempt, latched LOUD.
bool g_armed = false;
bool g_armFailedLatched = false;
std::atomic<bool> g_armedAtomic{false};

void* g_fnSeeWisps = nullptr;
void* g_fnCheckIfReached = nullptr;
void* g_fnRandLoc = nullptr;
void* g_fnChangeLook = nullptr;  // v102: the 1 Hz RANDOM head-wander re-roll (mirror-suppressed)
int32_t g_offWispTarget = -1;
int32_t g_isWalkingOff = -1;
uint8_t g_isWalkingMask = 0;
int32_t g_gatheringOff = -1;
uint8_t g_gatheringMask = 0;
// Facing axis (2026-07-05, "facing wrong direction" hands-on): the pyramid's visible heading
// lives in the movementVector/Arrow ArrowComponents' WORLD rotation (the actor root NEVER yaws
// -- WA-TRACE proved yaw=0.0 for the whole walk; the AnimBP orients the body off the component
// via its piramid2 back-ref). The host's Turning tick step RInterpTo's both components; the
// mirror's brain is suppressed so nothing turns them -> spawn-facing forever. RE:
// votv-piramid2-RE-2026-07-04.md section 2 step 4 + the sync-axis table's own answer for yaw:
// derive from streamed pos deltas -- the march is loc += movementVector.ForwardVector * speed,
// so velocity direction == heading BY CONSTRUCTION. No wire change.
int32_t g_offMovementVector = -1;
int32_t g_offArrow = -1;
int32_t g_offRelLook = -1;  // v102 head axis: `relLook` FVector (the lookat VInterpTo idle target)

// TLS allow slot for the client gather replay: our own re-dispatch of checkIfReached must
// pass the brain-suppress interceptor (the g_incoming* bypass shape, thread-local because
// the whole stage+call sequence is one inline game-thread block).
thread_local bool t_allowCheckIfReached = false;

// HOST gather edge detector: pyramid actor -> last-seen `gathering` (game-thread only; the
// checkIfReached dispatch is timer-fired). Swept 1 Hz against the live WA set so a recycled
// heap address can't inherit a stale `true` and eat a fresh pyramid's first relay.
std::unordered_map<void*, bool> g_lastGathering;

// CLIENT pending gather (single slot -- the host serializes gathers behind its own
// `gathering` latch, so a newer relay legitimately supersedes an unconverged older one).
struct PendingGather {
    uint32_t pyramidEid = 0;
    uint32_t wispEid = 0;
    long long deadlineMs = 0;
    bool active = false;
    float lastDist = -1.0f;  // last measured mirror 2D dist (deadline diagnostics)
    int attempts = 0;        // dispatch attempts (branch-not-taken retries)
};
PendingGather g_pending;

// CLIENT: pyramid mirror eids whose actor tick we restored (world_actor_sync parks generic
// mirrors tick-off; the pyramid's tick carries the beam params / look-at / hover smoothing).
std::unordered_set<uint32_t> g_tickRestored;

// (v100) The facing axis streams as WorldActorPoseSnapshot.auxYaw -- the HOST's true component
// heading at pose cadence. The earlier position-delta derivation was live-refuted 2026-07-05
// («сбивается иногда»): the native heading keeps RInterpTo-easing toward the walk target for up
// to 10 s AFTER motion stops (the mov timeline ramp), which no motion delta can observe.

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

// ---- brain suppression (client mirrors) ------------------------------------------------------
// PRE interceptor on the three STATE-WRITING timer handlers (every walkTo caller). Any
// client-side piramid2_C is a mirror: the client scheduler is dormant and world_actor_sync
// suppresses local allowlisted spawns, so a per-role gate IS the per-self gate for this class.
bool BrainSuppress_Interceptor(void* /*self*/, void* /*params*/) {
    auto* s = LoadSession();
    if (!s || !s->connected()) return false;                 // solo/pre-connect: native runs
    if (s->role() != coop::net::Role::Client) return false;  // host brain is authoritative
    if (t_allowCheckIfReached) return false;                 // our own gather replay dispatch
    return true;  // cancel the mirror's brain step
}

// Host actor -> lane identity maps. O(live elements) linear snapshots -- called only on a
// gather COMMIT edge (rare: once per consumed wisp).
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

// ---- host gather detect ----------------------------------------------------------------------
// POST observer on checkIfReached (1 Hz timer dispatch, game thread). The gather COMMIT is
// inside the function body (RE @6733: isWalking=false; wispTarget.gather; gathering=true;
// montage), so the POST reads the just-written `gathering` and edge-detects false->true.
// The montage-completed path resets gathering=false off this seam; the next 1 Hz dispatch's
// POST records the falling edge (no relay) and re-arms the detector for the next wisp.
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

// ---- arming ----------------------------------------------------------------------------------
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
        // An element with this typeName exists, so the class must be loaded -- a miss here is
        // name drift, which retrying can never heal.
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
    // Facing axis (auxiliary -- a miss disables only the heading drive, not the lane): the two
    // heading ArrowComponents the host's Turning step rotates (RE @0x330/@0x338).
    g_offMovementVector = R::FindPropertyOffset(cls, L"movementVector");
    g_offArrow = R::FindPropertyOffset(cls, L"Arrow");
    if (g_offMovementVector < 0)
        UE_LOGW("piramid-brain: movementVector offset unresolved -- client mirror facing will stay "
                "at spawn heading (name drift?)");
    // v102 head axis (auxiliary like the facing pair -- a miss disables only the head stream):
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
    // Register-all-or-disable: a partial set would half-suppress the mirror brain (worse than
    // none -- e.g. seeWisps alive + checkIfReached dead latches isWalking forever).
    const bool i1 = GT::RegisterInterceptor(g_fnSeeWisps, &BrainSuppress_Interceptor);
    const bool i2 = i1 && GT::RegisterInterceptor(g_fnCheckIfReached, &BrainSuppress_Interceptor);
    const bool i3 = i2 && GT::RegisterInterceptor(g_fnRandLoc, &BrainSuppress_Interceptor);
    // v102: changeLook joins the suppressed set -- its 1 Hz RANDOM relLook re-roll is the head
    // divergence; the streamed auxVec (host relLook) is the one writer on a mirror.
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

// ---- client mirror upkeep --------------------------------------------------------------------
// New pyramid mirror: undo anything its brain wrote in the pre-arm window (<=250 ms: a
// seeWisps could have latched isWalking + wispTarget before the interceptors existed), then
// restore its actor tick (beams / look-at / hover; march stays zero with isWalking cleared).
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

// Attempt the queued gather replay; retried every tick until converged or deadline.
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
        // Already mid-gather (duplicate relay / prior replay landed) -- nothing to stage.
        g_pending.active = false;
        return;
    }
    const auto pl = E::GetActorLocation(pyr);
    const auto wl = E::GetActorLocation(wisp);
    const float dx = pl.X - wl.X, dy = pl.Y - wl.Y;
    g_pending.lastDist = std::sqrt(dx * dx + dy * dy);
    if (g_pending.lastDist > kReplayAttemptRadius) return;  // interp still converging
    // Stage the native inputs the host had at its own commit, then run the game's own branch.
    // This whole block is one inline game-thread sequence -- ReceiveTick cannot interleave, so
    // the staged isWalking=true is only ever observed by the checkIfReached call below (which
    // resets it in its stop path before any tick could march on it).
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
        // Branch not taken (dist read >10000 inside the native re-check, or the frame failed):
        // unstage so the alive tick can never march on a stale latch, and RETRY next tick --
        // the mirrors are still converging toward the host's frozen <=10000 truth.
        WriteBoolAt(pyr, g_isWalkingOff, g_isWalkingMask, false);
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pyr) + g_offWispTarget) = nullptr;
        return;
    }
    g_replayCount.fetch_add(1, std::memory_order_relaxed);
    // The SUCK is the WISP's own tick code: killerwisp.gather() (which the native branch we
    // just re-dispatched called on THIS mirror wisp) latches gathered=true + MOVE_None and
    // snapshots the mesh start point; the rise itself is the wisp ReceiveTick's gathered
    // branch -- Mesh->K2_SetWorldLocation(VLerp(start, (piram2.p_L+p_R)/2, piram2.suc)) +
    // the center shrink [killerwisp uber @445-451/@409-411]. npc_mirror parks every
    // non-wisp_C NPC mirror tick-OFF, so without this the client wisp stays GROUNDED and
    // the pyramid's beams (tied to its center) stay long (user live 2026-07-05). Re-enabling
    // the actor tick here is safe: with gathered=true the hunt AI is unreachable (the uber's
    // tryGrab|grab|killed|gathered gate), CMC is MOVE_None, and every reachable branch is a
    // pure derivation of mirrored state -- the same principle as the pyramid mirror's own
    // alive ReceiveTick. The wisp dies seconds later at the montage's del notify.
    E::SetActorTickEnabled(wisp, true);
    UE_LOGI("piramid-gather[client]: replay OK -- native gather running on mirror; wisp mirror "
            "tick ENABLED for the gathered rise (pyramidEid=%u wispEid=%u dist=%.0f attempts=%d)",
            g_pending.pyramidEid, g_pending.wispEid, g_pending.lastDist, g_pending.attempts);
    g_pending.active = false;
}

// Host 1 Hz: drop edge-map entries whose actor is no longer a tracked pyramid (event ended;
// a recycled address must not inherit a stale `gathering=true`).
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

    // Client: restore-scan at 250 ms; pending replay every tick (cheap Get()s, rare).
    if (now - g_lastProbeMs >= 250) {
        g_lastProbeMs = now;
        RestoreNewClientMirrors();
    }
    TryReplayPendingGather();
}

bool ReadHostHeadingYaw(void* actor, float& outYaw) {
    // Pre-arm (the first ~250 ms after enroll) or an offset miss: caller falls back to the actor
    // yaw. No live-check here -- the caller (TickPoseStream) just IsLiveByIndex'd the actor.
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
    // All-zero = the host stream had no value (pre-arm / offset miss on the host side) --
    // keep the mirror's current target rather than yanking the head to the origin.
    if (x == 0.f && y == 0.f && z == 0.f) return;
    float* v = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(actor) + g_offRelLook);
    v[0] = x; v[1] = y; v[2] = z;
}

namespace {
// ReadHostWispTargetEid cache: the resolve walks the npc element table (heap snapshot), which
// is fine on the RARE gather edge but not at the 60 Hz pose stream. wispTarget changes on the
// 1 Hz seeWisps / gather / del cadence, so a single (ptr -> eid) memo collapses the per-tick
// cost to one pointer compare. Game thread only (TickPoseStream). A single entry suffices:
// the event spawns ONE pyramid (two alternating pyramids would degrade to a per-tick resolve,
// not break).
void*    g_lastHostWisp = nullptr;
uint32_t g_lastHostWispEid = 0;
}  // namespace

uint32_t ReadHostWispTargetEid(void* actor) {
    // v104 (the 0y "head/searchlight slightly desync" residue, user 2026-07-05 late): during
    // the WALK-to-wisp phase the host head eases toward wispTarget's WORLD location (tick
    // chase branch) while the mirror -- wispTarget nulled by design until the gather relay --
    // idled on relLook wander the host itself was ignoring. Stream the target IDENTITY so the
    // mirror runs the same native branch. 0 = no target (pre-arm / miss / genuinely idle).
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
    // While the mirror is mid-GATHER the field belongs to the gather choreography (the replay
    // staged it; the montage's del notify consumes + clears it natively). The host clears ITS
    // wispTarget on its OWN montage clock -- pushing that clear here mid-suck would strand the
    // mirror's del path. Hands off until the mirror's gathering falls.
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
    // Read CURRENT truth off the live actors (not the edge map -- it can lag the 1 Hz sweep):
    // a pyramid whose `gathering` is latched right now is mid-choreography; re-send the commit
    // to this joiner so its mirror replays it (the original relay fired before this peer
    // connected). The wisp may already be consumed (npc lane retired it) -- then the joiner
    // only misses the beam tail, and there is nothing valid to stage; skip.
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
    // v104 host wispTarget memo: a recycled actor address in a NEW session must not alias
    // the old session's eid.
    g_lastHostWisp = nullptr;
    g_lastHostWispEid = 0;
    // Probe counters are per-session: a piramidforce re-run in the SAME process after a
    // reconnect must not see the previous session's relays (correctness-audit nit 2026-07-04).
    g_relayCount.store(0, std::memory_order_relaxed);
    g_replayCount.store(0, std::memory_order_relaxed);
    // Hooks stay latched (role/session-gated inside) -- the npc/world_actor observer shape.
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
