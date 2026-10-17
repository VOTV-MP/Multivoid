// coop/dev/hookdrag_selftest.cpp -- see coop/dev/hookdrag_selftest.h.

#include "coop/dev/hookdrag_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"       // StartBackgroundWalk: the walk is the director's, never a teleport
#include "coop/dev/director/routes.h"         // ReachableStandpoints: a point a route reaches
#include "coop/dev/hook_plant.h"              // Plant: the hook planted on the prop, as the item does it
#include "coop/items/hook_sync.h"             // HasMirrorFrom, BittenEidFrom: the planter's hook, as this peer holds it
#include "coop/net/session.h"
#include "coop/player/hand_item.h"            // CollectHandAxisActors: the hand is not a world prop
#include "coop/player/players_registry.h"
#include "coop/player/sleep_sync.h"           // HostAsleepNow: the beside arm's start gate
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_wire_parity.h"      // PhysFlagsOf: the pick's physics flags, as the wire states them
#include "coop/props/prop_key_index.h"  // Find: the world prop a key names
#include "coop/props/remote_prop.h"           // ResolveMirrorEidByActor, ResolveLiveActorByEid: the eid a copy carries
#include "coop/props/prop_snapshot.h"  // IsBracketClosed: the host's end of a join
#include "coop/session/join_progress.h"  // CurrentPhase: the client's end of a join
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::hookdrag_selftest {
namespace {

namespace E  = ue_wrap::engine;
namespace H  = ue_wrap::hook;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::hookdrag_selftest);
    return s;
}

// Who plants: the host (the driven-prop channel alone), the client (the host ties the client's
// hook on its mirror and the channel carries the result back), or both into the same prop (two
// host constraints on one body). Read once; the same value is set on both peers.
const std::string& Role() {
    static const std::string s = coop::config::ResolveEnum(::coop::config_registry::rows::hookdrag_role);
    return s;
}
bool PlantsHere(bool isHost) {
    const std::string& r = Role();
    if (r == "both") return true;
    return (r == "client") ? !isHost : isHost;
}

// The sleep drill's `beside` arm runs this drill under a sleeping host: the origin waits for the
// sleep, on both peers (a client reads the host's flag). Read once from the same row on this peer.
bool BesideArmed() {
    static const bool s =
        coop::config::ResolveEnum(::coop::config_registry::rows::sleep_drill) == "beside";
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The phases, each ended by what happened and not by a count of seconds. The origin is the first
// tick at which BOTH peers are in the world, the shared point the disc driver uses and for the same
// reason: the two connect clocks sit about ten seconds apart. Every phase has a bound, the backstop
// that turns a phase that never comes into a FAIL line naming it; no bound is a schedule.
//   planter: Pick -> Plant -> WalkOut -> WalkBack -> Unhook -> Rest -> Ended
//   other:   Pick -> Tie -> Release -> Rest -> Ended   (the planter's hook held here, then gone, then the prop at rest)
// The planter picks the prop nearest itself; the other peer takes the prop the planter's hook bit.
enum class Phase { Pick, Plant, WalkOut, WalkBack, Unhook, Tie, Release, Rest, Ended };

constexpr uint64_t kSampleMs     =   250;   // the position log, both roles
constexpr float    kPickRadiusCm =  1500.f;
// The leg out ends at a point a NavMesh route reaches that is this much farther from the prop than the
// walker stood when it planted, so the cable (the length it had at the plant) is overrun by at least this.
constexpr float    kWalkCm       =  1200.f;
constexpr float    kArriveCm     =   100.f; // a leg ends this close to its point (the runs on record
                                            // ended 7..22 cm from the start they walked back to)
constexpr float    kRestCm       =     1.f; // a sample this close to the one before it (4 cm/s) is a prop at
                                            // rest: at 5 cm (20 cm/s) a prop still creeping down a slope
                                            // read as rested and the peer's last sample was not its rest ...
constexpr int      kRestSamples  =      12; // ... and this many in a row (3 s) end the run: the verdict's "final"
                                            // reads the last 3 s, which must then be rest, not the coast
// The director's own bound on one leg: a fixed share and the straight length at a pace under half the
// walk's own (kerfus_throw_drill's rule).
constexpr int      kWalkDeadlineS = 60;
constexpr float    kWalkFloorCmS  = 200.f;
// The backstops. Plant and Unhook are one dispatch each: their event arrives in the tick they start.
constexpr uint64_t kPickBoundMs  =  60000;  // the planter: the join is over and the player reads
constexpr uint64_t kTieBoundMs   =  90000;  // the peer that does not plant: the other's pick and plant
// One leg: the director's deadline for the longest straight leg the pick radius allows (a ring point is
// at most 2 * kPickRadiusCm + kWalkCm from the walker, 4200 cm, so 81 s) and a margin.
constexpr uint64_t kWalkBoundMs  =  90000;
constexpr uint64_t kRestBoundMs  =  60000;
// The peer that does not plant: the planter's two legs, then its unhook reaching here.
constexpr uint64_t kReleaseBoundMs = 2 * kWalkBoundMs + 30000;

uint64_t g_originMs     = 0;
uint64_t g_nextSampleMs = 0;
uint64_t g_phaseMs      = 0;                // when the current phase began
Phase    g_phase = Phase::Pick;
int      g_restRun = 0;                     // consecutive samples within kRestCm of the one before
const char* g_waitWhy = "";                 // what the current phase is waiting on, for its FAIL line
float    g_waitCm = -1.f;                   // the last distance it read while waiting, or -1
bool     g_havePrev = false;
ue_wrap::FVector g_prevLoc{};
ue_wrap::CachedObjRef g_target;
ue_wrap::CachedObjRef g_hook;
std::wstring          g_targetKey;
ue_wrap::FVector      g_targetStart{};
ue_wrap::FVector      g_walkBase{}, g_legTo{};   // where the planter stood, and the point the current leg walks to
std::shared_ptr<coop::director::BackgroundWalk> g_walk;   // the director's walk of the current leg
float                 g_maxMovedCm = 0.f;
float                 g_goneAtS    = -1.f;   // when the target died under the sampler, or -1
float                 g_unreadAtS  = -1.f;   // when the target's location stopped reading, or -1
const char*           g_plantNote  = "not attempted";

const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Pick:     return "pick";
    case Phase::Plant:    return "plant";
    case Phase::WalkOut:  return "walk out";
    case Phase::WalkBack: return "walk back";
    case Phase::Unhook:   return "unhook";
    case Phase::Tie:      return "tie";
    case Phase::Release:  return "release";
    case Phase::Rest:     return "rest";
    case Phase::Ended:    return "ended";
    }
    return "?";
}

// 0: the phase waits on nothing, so it has no bound.
uint64_t BoundMs(Phase p, bool planter) {
    switch (p) {
    case Phase::Pick:     return planter ? kPickBoundMs : kTieBoundMs;
    case Phase::WalkOut:
    case Phase::WalkBack: return kWalkBoundMs;
    case Phase::Tie:      return kTieBoundMs;
    case Phase::Release:  return kReleaseBoundMs;
    case Phase::Rest:     return kRestBoundMs;
    default:              return 0;
    }
}

void EnterPhase(Phase p, uint64_t now) {
    g_phase = p;
    g_phaseMs = now;
    g_waitWhy = "";
    g_waitCm = -1.f;
}

void* LocalPlayer() {
    void* p = coop::players::Registry::Get().Local();
    return (p && R::IsLive(p)) ? p : nullptr;
}

// Where the planter looks for its target: at its own player, host or client, since the prop it
// hooks is the one it can reach.
bool Anchor(ue_wrap::FVector& out, const char*& why) {
    void* p = LocalPlayer();
    if (!p) { why = "this player is not here yet"; return false; }
    why = "this player's location could not be read";
    return E::TryGetActorLocation(p, out);
}

// The nearest keyed, simulating, light Aprop_C to `anchor` within the radius: one object-array
// walk, once per run, the nearest-prop shape with the props that test IsStatic, IsFrozen or
// IsHeavy left out (a sleeping prop still passes; the TARGET line logs the physics flags), since a
// hook drags none of them cleanly. Two more exclusions make it a WORLD prop: the hand axis (the local hotbar hand
// and every peer's display mirror stand at a player's hands and follow the player whether or not
// anything drags them), and membership in the tracked-prop key index, which is what the census
// gives a world prop and withholds from a hand actor. Only the planter runs it; the other peer
// takes the planter's prop by the element id the planter's hook names.
void* PickTarget(const ue_wrap::FVector& anchor, std::wstring& keyOut, ue_wrap::FVector& locOut) {
    void* best = nullptr;
    float bestD2 = kPickRadiusCm * kPickRadiusCm;
    void* handAxis[1 + coop::players::kMaxPeers];
    const size_t handAxisN =
        coop::hand_item::CollectHandAxisActors(handAxis, 1 + coop::players::kMaxPeers);
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !PR::IsDescendantOfProp(obj)) continue;
        bool onHand = false;
        for (size_t h = 0; h < handAxisN; ++h) if (handAxis[h] == obj) { onHand = true; break; }
        if (onHand) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        if (PR::IsStatic(obj) || PR::IsFrozen(obj) || PR::IsHeavy(obj)) continue;
        const std::wstring key = PR::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") continue;
        if (coop::prop_key_index::Find(key) != obj) continue;  // not the world prop its key names
        ue_wrap::FVector loc{};
        if (!E::TryGetActorLocation(obj, loc)) continue;   // unreadable: never the target
        const float dx = loc.X - anchor.X, dy = loc.Y - anchor.Y, dz = loc.Z - anchor.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = obj; keyOut = key; locOut = loc; }
    }
    return best;
}

// The shared clock the two peers' samples are aligned on: both run on one machine, so the system
// clock is common to the millisecond, where each peer's own origin is a second apart.
uint64_t WallMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void Sample(uint64_t since, bool isHost) {
    void* t = g_target.Get();
    if (!t) {
        // Said once; the sampling stops with the target, and the verdict carries the time.
        g_goneAtS = static_cast<float>(since / 1000.0);
        UE_LOGW("hookdrag_selftest: POS t=%.2f key='%ls' -- the target is GONE; sampling stops",
                since / 1000.0, g_targetKey.c_str());
        return;
    }
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(t, loc)) {
        // Said once, like a gone target: the read repeats, so sampling stops and the verdict carries the time.
        g_unreadAtS = static_cast<float>(since / 1000.0);
        UE_LOGW("hookdrag_selftest: POS t=%.2f key='%ls' -- the target's location could not be read; sampling stops",
                since / 1000.0, g_targetKey.c_str());
        return;
    }
    const float dx = loc.X - g_targetStart.X, dy = loc.Y - g_targetStart.Y, dz = loc.Z - g_targetStart.Z;
    const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (moved > g_maxMovedCm) g_maxMovedCm = moved;
    // The rest the last phase waits on: samples in a row that stood where the one before did.
    if (g_havePrev) {
        const float px = loc.X - g_prevLoc.X, py = loc.Y - g_prevLoc.Y, pz = loc.Z - g_prevLoc.Z;
        g_restRun = std::sqrt(px * px + py * py + pz * pz) < kRestCm ? g_restRun + 1 : 0;
    }
    g_prevLoc = loc;
    g_havePrev = true;
    // The walker's own position beside the prop's, so a drag that stops short can be read as the
    // player stopping (a wall, or the hook's own pull on the player winning over the walk) rather
    // than guessed. The local player on both peers: on the non-planting peer it is a bystander.
    ue_wrap::FVector me{};
    void* self = LocalPlayer();
    const bool meRead = self && E::TryGetActorLocation(self, me);
    UE_LOGI("hookdrag_selftest: POS t=%.2f wall=%llu key='%ls' (%.1f,%.1f,%.1f) moved=%.1f role=%s "
            "me=(%.1f,%.1f,%.1f)%s",
            since / 1000.0, static_cast<unsigned long long>(WallMs()), g_targetKey.c_str(),
            loc.X, loc.Y, loc.Z, moved, isHost ? "HOST" : "CLIENT", me.X, me.Y, me.Z, meRead ? "" : " (unread)");
}


// Is this peer's end of the join over: the host's bracket for the joiner closed, the client's join
// progress idle. The pick reads props the join delivers, so it waits for the join.
bool JoinOver(bool isHost) {
    return isHost ? coop::prop_snapshot::IsBracketClosed(1)
                  : coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// The local player's horizontal distance from the current leg's point; false while its location is unread.
bool LegDistance(float& out) {
    void* p = LocalPlayer();
    ue_wrap::FVector me{};
    if (!p || !E::TryGetActorLocation(p, me)) return false;
    const float dx = me.X - g_legTo.X, dy = me.Y - g_legTo.Y;
    out = std::sqrt(dx * dx + dy * dy);
    return true;
}

// One leg, walked by the director: a NavMesh route taken as movement input at the human seam, bounded
// by the director's own deadline. The run reads the leg's end from the walk's state, 1 on arrival.
// A carrying walk: the planter's hand may hold the hook tool its planted hook is tied to, and the
// plain walk set's ClearHand would throw it at the first tick and kill the hook.
void StartLeg(const ue_wrap::FVector& to) {
    g_legTo = to;
    float len = 0.f;
    LegDistance(len);
    g_walk = coop::director::StartBackgroundWalk(to, kArriveCm,
                                                 kWalkDeadlineS + static_cast<int>(len / kWalkFloorCmS),
                                                 /*carry=*/true);
}

// How the current leg stands: 0 still walking, 1 arrived, 2 ended without arriving (the director's
// failure, its deadline, or no walk started).
int LegState() { return g_walk ? g_walk->state.load() : 2; }

// The leg out: to a point a route reaches that stands kWalkCm farther from the prop than the walker
// does now (the ring about the prop at its distance plus kWalkCm: by the triangle inequality the
// point is at least kWalkCm from the walker and from the prop, and the cable, which has the length
// it had at the plant, is overrun by that much). False when the walker's or the prop's position is
// unread or no route reaches such a point; `why` names which.
bool BeginWalkOut(void* player, void* target, const char*& why) {
    if (!E::TryGetActorLocation(player, g_walkBase)) { why = "the walker's position could not be read"; return false; }
    ue_wrap::FVector tl{};
    if (!E::TryGetActorLocation(target, tl)) { why = "the prop's position could not be read"; return false; }
    const float dx = g_walkBase.X - tl.X, dy = g_walkBase.Y - tl.Y;
    const float ring = std::sqrt(dx * dx + dy * dy) + kWalkCm;
    const std::vector<ue_wrap::FVector> points =
        coop::director::ReachableStandpoints(player, tl, ring, 1, kArriveCm);
    if (points.empty()) { why = "no NavMesh route reaches a point beyond the prop's cable"; return false; }
    UE_LOGI("hookdrag_selftest: WALK OUT by the director to (%.1f,%.1f,%.1f), %.0f cm from the prop, from "
            "(%.1f,%.1f,%.1f)", points[0].X, points[0].Y, points[0].Z, ring, g_walkBase.X, g_walkBase.Y,
            g_walkBase.Z);
    StartLeg(points[0]);
    return true;
}

// The item's cancel: the field this driver wrote is cleared whether or not the hook still stands,
// then the actor goes. True when the hook still stood.
bool ReleaseHook() {
    void* h = g_hook.Get();
    coop::dev::hook_plant::Release(LocalPlayer(), h);
    g_hook.Reset();
    return h != nullptr;
}

// The peer that does not plant holds the planter's hook as that slot's: the joiner's for the host,
// the host's for a client.
int PlanterSlot(bool isHost) { return isHost ? 1 : 0; }

// The run is over for this peer: the verdict line the driver waits on, said once.
void End() {
    g_phase = Phase::Ended;
    EmitVerdict();
}

// A phase did not come, or could not be measured: the run ends at it, naming it.
void Fail(bool isHost, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    // A planted hook does not outlive the run that failed, nor the host's constraint on it.
    if (g_hook.Raw()) ReleaseHook();
    // The walker stops where it stands: a leg still walking would move the player after the verdict.
    if (g_walk) {
        coop::director::CancelBackgroundWalk(*g_walk);
        g_walk.reset();
    }
    UE_LOGW("hookdrag_selftest: FAIL role=%s phase=%s -- %s", isHost ? "HOST" : "CLIENT", PhaseName(g_phase), buf);
    End();
}

// The target is chosen: `t` is the world prop this run measures. Said once per run, with the class and
// the physics flags the pick has, so a run that names a prop that cannot move is readable from the log.
void TakeTarget(bool isHost, void* t, const char* how, uint64_t now) {
    g_target.Set(t);
    // The eid is the cross-peer identity: the host's local element, the client's mirror
    // row from the join snapshot. Two keys can differ where two eids agree.
    const coop::element::ElementId eid =
        isHost ? coop::prop_element_tracker::GetPropElementIdForActor(t)
               : coop::remote_prop::ResolveMirrorEidByActor(t);
    UE_LOGI("hookdrag_selftest: TARGET role=%s key='%ls' eid=%u class=%ls flags=0x%02x at (%.1f,%.1f,%.1f) -- %s",
            isHost ? "HOST" : "CLIENT", g_targetKey.c_str(),
            eid == coop::element::kInvalidId ? 0u : static_cast<unsigned>(eid),
            R::ToString(R::NameOf(R::ClassOf(t))).c_str(),
            static_cast<unsigned>(coop::prop_wire_parity::PhysFlagsOf(t)),
            g_targetStart.X, g_targetStart.Y, g_targetStart.Z, how);
    g_nextSampleMs = now;
    EnterPhase(PlantsHere(isHost) ? Phase::Plant : Phase::Tie, now);
}

}  // namespace

uint64_t WorstCaseMs(bool isHost) {
    const bool planter = PlantsHere(isHost);
    const Phase path[] = {Phase::Pick, planter ? Phase::WalkOut : Phase::Tie,
                          planter ? Phase::WalkBack : Phase::Release, Phase::Rest};
    uint64_t sum = 0;
    for (const Phase p : path) sum += BoundMs(p, planter);
    return sum;
}

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
    // The subsystem install list is a per-tick ensure, so the line is said once.
    static bool sSaid = false;
    if (sSaid) return;
    sSaid = true;
    UE_LOGI("hookdrag_selftest: ENABLED (role=%s) -- the planting peer hooks the nearest keyed "
            "physics prop to itself and drags it; the other peer takes the prop that hook bites; both "
            "peers log the prop's position", Role().c_str());
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || g_phase == Phase::Ended) return;
    const bool isHost = s->role() == coop::net::Role::Host;
    const uint64_t now = NowMs();
    if (!g_originMs) {
        // The beside arm holds the HOST asleep, and a sleeping host cannot plant: only the client may.
        if (BesideArmed() && Role() != "client")
            return Fail(isHost, "sleep_drill=beside needs hookdrag_role=client: the sleeping host cannot "
                                "plant (role=%s)", Role().c_str());
        // Only a CLIENT announces ClientWorldReady; the host learns the same fact as that slot
        // going world-ready.
        if (isHost ? !s->IsSlotWorldReady(1) : !coop::net_pump::HasAnnouncedWorldReady()) return;
        if (BesideArmed() && !coop::sleep_sync::HostAsleepNow()) return;
        g_originMs = now;
        EnterPhase(Phase::Pick, now);
        if (PlantsHere(isHost))
            UE_LOGI("hookdrag_selftest: ARMED role=%s plants=1 (each phase ends on its event: pick the "
                    "nearest keyed prop to this player, plant, the director's walk %.0f cm past the prop's "
                    "cable and back to within %.0f cm of the start, unhook, rest)",
                    isHost ? "HOST" : "CLIENT", kWalkCm, kArriveCm);
        else
            UE_LOGI("hookdrag_selftest: ARMED role=%s plants=0 (each phase ends on its event: take the "
                    "prop the planter's hook bites, hold the planter's hook, its release, rest)",
                    isHost ? "HOST" : "CLIENT");
    }
    const uint64_t since = now - g_originMs;
    if (const uint64_t bound = BoundMs(g_phase, PlantsHere(isHost)); bound && now - g_phaseMs > bound) {
        if (g_waitCm >= 0.f)
            return Fail(isHost, "no event within %llu s: %s (last read %.0f cm)",
                        static_cast<unsigned long long>(bound / 1000), g_waitWhy, g_waitCm);
        return Fail(isHost, "no event within %llu s: %s", static_cast<unsigned long long>(bound / 1000), g_waitWhy);
    }
    if (!H::EnsureResolved()) return;

    if (g_target.Raw() && g_goneAtS < 0.f && g_unreadAtS < 0.f && now >= g_nextSampleMs) {
        g_nextSampleMs = now + kSampleMs;
        Sample(since, isHost);
    }
    if (g_target.Raw() && (g_goneAtS >= 0.f || g_unreadAtS >= 0.f))
        return Fail(isHost, "the target is %s; the series stops there", g_goneAtS >= 0.f ? "gone" : "unreadable");

    switch (g_phase) {
    case Phase::Pick: {
        // The planter's pick ends when it succeeds. One shot: it waits for the join and its own
        // position, and a miss after that is final. The other peer does not pick: it takes the prop
        // the planter's hook bites, by the element id that hook's state names, and waits for it.
        g_waitWhy = "the join is not over";
        if (!JoinOver(isHost)) return;
        char how[96];
        if (PlantsHere(isHost)) {
            ue_wrap::FVector anchor{};
            const char* why = "";
            if (!Anchor(anchor, why)) { g_waitWhy = why; return; }
            void* t = PickTarget(anchor, g_targetKey, g_targetStart);
            if (!t) return Fail(isHost, "no keyed physics prop within %.0f cm", kPickRadiusCm);
            const float dx = g_targetStart.X - anchor.X, dy = g_targetStart.Y - anchor.Y,
                        dz = g_targetStart.Z - anchor.Z;
            std::snprintf(how, sizeof(how), "%.0f cm from this player",
                          std::sqrt(dx * dx + dy * dy + dz * dz));
            return TakeTarget(isHost, t, how, now);
        }
        g_waitWhy = "the planter's hook has not named a prop here";
        const uint32_t eid = coop::hook_sync::BittenEidFrom(PlanterSlot(isHost));
        if (eid == 0) return;
        g_waitWhy = "the prop the planter's hook bites is not here";
        void* t = coop::remote_prop::ResolveLiveActorByEid(eid);
        ue_wrap::FVector loc{};
        if (!t || !E::TryGetActorLocation(t, loc)) return;
        g_targetKey = PR::GetInteractableKeyString(t);
        g_targetStart = loc;
        std::snprintf(how, sizeof(how), "the prop the planter's hook bites (eid %u)", static_cast<unsigned>(eid));
        return TakeTarget(isHost, t, how, now);
    }
    case Phase::Plant: {
        // Ends when the attach dispatched. A client's tie is built on the host and nothing of it
        // reaches this peer, so the host's own log carries that line.
        void* p = LocalPlayer();
        void* t = g_target.Get();
        if (!p || !t) g_plantNote = !p ? "no-local-player" : "target-gone";
        void* hook = (p && t) ? coop::dev::hook_plant::Plant(p, t, &g_plantNote) : nullptr;
        if (!hook) return Fail(isHost, "%s", g_plantNote);
        g_hook.Set(hook);
        UE_LOGI("hookdrag_selftest: PLANT key='%ls' hook=%p -- attach_a with the head on the "
                "prop and the tail on the player", g_targetKey.c_str(), g_hook.Raw());
        const char* walkWhy = "";
        if (!BeginWalkOut(p, t, walkWhy)) return Fail(isHost, "%s", walkWhy);
        EnterPhase(Phase::WalkOut, now);
        return;
    }
    case Phase::WalkOut:
        // Ends when the director's walk arrives at the point past the prop's cable; its failure or
        // its own deadline is a FAIL naming the leg.
        g_waitWhy = "the director's walk out did not arrive";
        LegDistance(g_waitCm);
        if (const int walk = LegState(); walk != 0) {
            if (walk == 2) {
                if (g_waitCm >= 0.f)
                    return Fail(isHost, "the director's walk out ended without arriving (%.0f cm from its point)",
                                g_waitCm);
                return Fail(isHost, "the director's walk out ended without arriving");
            }
            UE_LOGI("hookdrag_selftest: WALK BACK by the director to the start (%.1f,%.1f,%.1f) -- the leg "
                    "out arrived", g_walkBase.X, g_walkBase.Y, g_walkBase.Z);
            StartLeg(g_walkBase);
            EnterPhase(Phase::WalkBack, now);
        }
        return;
    case Phase::WalkBack:
        // Ends when the director's walk arrives back within kArriveCm of where the walker planted.
        g_waitWhy = "the director's walk back did not arrive";
        LegDistance(g_waitCm);
        if (const int walk = LegState(); walk != 0) {
            if (walk == 2) {
                if (g_waitCm >= 0.f)
                    return Fail(isHost, "the director's walk back ended without arriving (%.0f cm from the start)",
                                g_waitCm);
                return Fail(isHost, "the director's walk back ended without arriving");
            }
            g_walk.reset();
            EnterPhase(Phase::Unhook, now);
        }
        return;
    case Phase::Unhook:
        // Ends when the hook is destroyed.
        if (ReleaseHook())
            UE_LOGI("hookdrag_selftest: UNHOOK -- the hook is destroyed; the prop coasts and rests");
        else
            UE_LOGW("hookdrag_selftest: UNHOOK -- the hook had already died");
        g_restRun = 0;
        EnterPhase(Phase::Rest, now);
        return;
    case Phase::Tie:
        // The peer that does not plant: ends when the planter's hook is tied to what it bit. The host
        // holds its own constraint on the client's hook; a client holds the host's hook mirror.
        g_waitWhy = isHost ? "the host did not tie the client's hook" : "the host's hook did not appear here";
        if (!coop::hook_sync::HasMirrorFrom(PlanterSlot(isHost), /*tied=*/isHost)) return;
        UE_LOGI("hookdrag_selftest: HOOK %s -- the planter's hook is held here", isHost ? "TIED" : "MIRRORED");
        EnterPhase(Phase::Release, now);
        return;
    case Phase::Release:
        // The peer that does not plant: ends when the planter's unhook has reached it.
        g_waitWhy = "the planter's hook was not destroyed";
        if (coop::hook_sync::HasMirrorFrom(PlanterSlot(isHost), /*tied=*/false)) return;
        UE_LOGI("hookdrag_selftest: HOOK GONE -- the planter's hook is destroyed; the prop coasts and rests");
        g_restRun = 0;
        EnterPhase(Phase::Rest, now);
        return;
    case Phase::Rest:
        // Ends when the prop has stood still for kRestSamples samples since the hook's destruction.
        g_waitWhy = "the prop did not come to rest";
        if (g_restRun < kRestSamples) return;
        UE_LOGI("hookdrag_selftest: REST -- the prop stood still for %d samples", g_restRun);
        End();
        return;
    case Phase::Ended:
        return;
    }
}

void EmitVerdict() {
    if (!Enabled() || !g_originMs) return;
    auto* s = g_session.load(std::memory_order_acquire);
    const bool isHost = s && s->role() == coop::net::Role::Host;
    UE_LOGI("hookdrag_selftest: VERDICT role=%s key='%ls' plant=%s maxMoved=%.1f cm goneAt=%.2f unreadAt=%.2f",
            isHost ? "HOST" : "CLIENT", g_targetKey.c_str(),
            PlantsHere(isHost) ? g_plantNote : "n/a", g_maxMovedCm, g_goneAtS, g_unreadAtS);
}

bool IsEnabled() { return Enabled(); }

bool HostEnded() {
    return Enabled() && g_phase == Phase::Ended;
}

void OnDisconnect() {
    g_originMs = 0;
    g_nextSampleMs = 0;
    g_phaseMs = 0;
    g_phase = Phase::Pick;
    g_restRun = 0;
    g_havePrev = false;
    g_waitWhy = "";
    g_waitCm = -1.f;
    g_target.Reset();
    g_walk.reset();
    g_walkBase = ue_wrap::FVector{};
    g_legTo = ue_wrap::FVector{};
    g_hook.Reset();
    g_targetKey.clear();
    g_maxMovedCm = 0.f;
    g_goneAtS    = -1.f;
    g_unreadAtS  = -1.f;
    g_plantNote  = "not attempted";
}

}  // namespace coop::dev::hookdrag_selftest
