// coop/dev/kerfus_throw_drill.cpp -- see coop/dev/kerfus_throw_drill.h.

#include "coop/dev/kerfus_throw_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_place.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_physics.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

namespace coop::dev::kerfus_throw_drill {
namespace {

namespace D  = coop::director;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace UK = ue_wrap::kerfus;

constexpr uint8_t  kActionToggle  = 8;        // the Kerfus's on/off
constexpr float    kReachCm       = 150.f;    // the use key's trace takes it from here
constexpr int      kWalkDeadlineS = 60;
// The walks to the Kerfus start from the spots the verified runs used (drill_place.cpp): the walk is a means, the drill
// judges the grab and the throw. The host's spot is where its 23 m walk to the Kerfus begins.
constexpr drill_place::Spot kHostSpot{-1692.f, 560.f, 6318.f, 102.3f};
constexpr drill_place::Spot kClientSpot{-535.f, 310.f, 6665.f, 0.f};
constexpr float    kWalkFloorCmS  = 200.f;    // a walk's deadline grows by its straight length at under half the pace
constexpr uint64_t kStateWindowMs = 10000;
constexpr uint64_t kHoldMs        = 1500;     // a player holds it a moment before letting go
constexpr float    kAimPitchDeg   = 10.f;     // the throw leaves slightly upward
constexpr float    kAimMinCm      = 400.f;    // the pile the host turns to: 4 to 10 m off, nav-reachable
constexpr float    kAimMaxCm      = 1000.f;
constexpr float    kRestCmS       = 15.f;     // resting below this speed
constexpr uint64_t kRestHoldMs    = 1500;     // for this long
constexpr uint64_t kRestWindowMs  = 25000;
constexpr uint64_t kSampleMs      = 50;
constexpr uint64_t kSnapWindowMs  = 300;      // a grab's snap lands inside this
constexpr int      kClientGrabs   = 2;
// The verdicts. A grab that moves the copy farther found it strayed from the host's. A Kerfus's centre of mass sits
// within about 60 cm of its origin, its on-state nudge included. Its navigation pawn rides the body: it moves on
// the body only when something moves it there.
constexpr float    kStrayCm       = 300.f;
constexpr float    kComCm         = 150.f;
constexpr float    kRiderCm       = 10.f;

struct Throw {
    bool on;
    bool fire;   // the fire key's throw; else the use key's release
};
constexpr Throw kPlan[] = {{false, false}, {false, true}, {false, true}, {true, false}, {true, true}, {true, true}};
constexpr int kThrows = static_cast<int>(sizeof(kPlan) / sizeof(kPlan[0]));

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_saidArm = false;

bool IsEnabled_() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::kerfus_throw_drill);
    return s;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Len(const ue_wrap::FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

// A director walk from `player` to `to`, its deadline grown with the distance: a joiner can start far off.
std::shared_ptr<D::BackgroundWalk> WalkTo(void* player, const ue_wrap::FVector& to) {
    ue_wrap::FVector from{};
    E::TryGetActorLocation(player, from);
    return D::StartBackgroundWalk(to, kReachCm, kWalkDeadlineS + static_cast<int>(Dist(from, to) / kWalkFloorCmS));
}

// ---- the Kerfus's shape: its centre of mass on the body, and the navigation pawn welded on it ----

// This peer's own Kerfus, sampled against where its pawn sat at the first sample.
struct Shape {
    ue_wrap::FVector rider0{};
    bool haveRider0 = false;
};

// One sample of `k`: `com` the centre of mass's distance from the body's origin, `riderMove` how far the pawn's
// capsule moved on the body since the first sample. False when a part does not read.
bool Sample(void* k, Shape& shape, float& com, float& riderMove) {
    void* mesh = ue_wrap::prop::GetStaticMesh(k);
    ue_wrap::FVector c{}, at{};
    if (!mesh || !E::GetComponentCenterOfMass(mesh, c) || !E::TryGetActorLocation(k, at)) return false;
    com = Dist(c, at);
    E::AttachedCharacter rider;
    if (E::AttachedCharactersOf(k, &rider, 1) != 1 || !rider.updated) return false;
    const ue_wrap::FVector rel = E::GetComponentRelativeLocation(rider.updated);
    if (!shape.haveRider0) {
        shape.rider0 = rel;
        shape.haveRider0 = true;
    }
    riderMove = Dist(rel, shape.rider0);
    return true;
}

// The shape at the start, once: which Character the walk finds on the body and whether it is the Kerfus's own
// pawn, whether its capsule answers the body's simulation (welded), where it sits, whether its movement ticks.
bool SayShape(const char* who, void* k) {
    E::AttachedCharacter rider;
    const int n = E::AttachedCharactersOf(k, &rider, 1);
    void* mesh = ue_wrap::prop::GetStaticMesh(k);
    ue_wrap::FVector c{}, at{};
    const bool comRead = mesh && E::GetComponentCenterOfMass(mesh, c) && E::TryGetActorLocation(k, at);
    if (n != 1 || !rider.updated || !comRead) {
        UE_LOGW("[KERFUS-THROW] %s: the Kerfus's shape does not read (%d Characters on it, centre of mass %s)", who,
                n, comRead ? "read" : "unread");
        return false;
    }
    const ue_wrap::FVector rel = E::GetComponentRelativeLocation(rider.updated);
    UE_LOGI("[KERFUS-THROW] %s: the Kerfus carries '%ls' (%s its navigation pawn); its capsule %s with the body, "
            "sits at (%.1f,%.1f,%.1f) on it, its movement %s; the centre of mass is %.1f cm from the origin", who,
            R::ClassNameOf(rider.character).c_str(), rider.character == UK::NavPawn(k) ? "" : "NOT",
            E::IsComponentSimulatingPhysics(rider.updated) ? "simulates" : "does not simulate", rel.X, rel.Y, rel.Z,
            E::IsComponentTickEnabled(rider.movement) ? "ticking" : "not ticking", Dist(c, at));
    return true;
}

// ---- host ----

enum class HostStep { WaitJoin, SetState, WaitState, Place, Walk, Grab, Hold, Rest, Watch, Idle };
HostStep g_host = HostStep::WaitJoin;
int g_throw = 0;   // the throw in hand; kThrows is the checking grab
ue_wrap::CachedObjRef g_hostKerfus;
std::unique_ptr<D::AimedGrab> g_grab;
std::shared_ptr<D::BackgroundWalk> g_walk;
drill_place::Placement g_hostPlace;   // the host's placement, once per drill, before its first walk
bool g_hostPlaced = false;
ue_wrap::FRotator g_aim{};
uint64_t g_stepMs = 0, g_sampleMs = 0, g_restSince = 0;
Shape g_hostShape;

// The client's grabs as the host's own Kerfus saw them, one entry per hold: through the park its receiver made
// and the rest after it.
struct Parked {
    float comMax = 0.f;
    float riderMax = 0.f;
};
Parked g_parked[kClientGrabs];
int g_remoteHolds = 0;
bool g_remoteHeld = false;

void HostInvalid(const char* why) {
    g_host = HostStep::Idle;
    UE_LOGW("[KERFUS-THROW] INVALID (host) -- throw %d: %s", g_throw, why);
}

bool WantOn() { return g_throw < kThrows ? kPlan[g_throw].on : false; }

void WalkToKerfusNow(void* player, void* k) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(k, at)) { HostInvalid("the Kerfus's place did not read"); return; }
    g_walk = WalkTo(player, at);
    g_host = HostStep::Walk;
}

// The host is placed at kHostSpot before its first walk; later walks start from where the host stands.
void StartWalkToKerfus(void* player, void* k) {
    if (!g_hostPlaced) { g_host = HostStep::Place; return; }
    WalkToKerfusNow(player, k);
}

// The aim for the throw: toward a nav-reachable pile 4 to 10 m off, a little upward, so it lands on floor the
// director can walk to; without such a pile, the way the player faces.
void PickAim(void* player) {
    void* ctrl = E::GetController(player);
    g_aim = E::GetControlRotation(ctrl);
    D::DirectorGoal goal;
    if (D::PickReachablePile(player, kAimMinCm, kAimMaxCm, goal)) {
        g_aim = D::LookAt(E::GetCameraLocation(), goal.targetPos);
        UE_LOGI("[KERFUS-THROW] host: throw %d aims at the pile at (%.0f,%.0f,%.0f)", g_throw, goal.targetPos.X,
                goal.targetPos.Y, goal.targetPos.Z);
    } else {
        UE_LOGI("[KERFUS-THROW] host: throw %d -- no pile 4 to 10 m off; it goes the way I face", g_throw);
    }
    g_aim.Pitch = kAimPitchDeg;
    E::SetControlRotation(ctrl, g_aim);
}

// The fire key's press as the input system delivers it: the player's own handler throws what it holds
// (traceThrow, throwShit, thrown, dropGrabObject).
bool PressFire(void* player) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(player), P::name::MainPlayerFireInputEventFn58);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(player, f);
}

void LetGo(void* player, void* k, uint64_t ms) {
    const bool fire = g_throw < kThrows && kPlan[g_throw].fire;
    if (!(fire ? PressFire(player) : D::CallOnPlayer(player, L"dropGrabObject"))) {
        HostInvalid(fire ? "the fire key's handler did not run" : "dropGrabObject did not run");
        return;
    }
    if (D::Grabbing(player) == k) { HostInvalid("the hand still holds it"); return; }
    ue_wrap::FVector at{};
    E::TryGetActorLocation(k, at);
    UE_LOGI("[KERFUS-THROW] host: throw %d (%s, by the %s key) left the hand at %.0f cm/s from (%.0f,%.0f,%.0f)",
            g_throw, WantOn() ? "on" : "off", fire ? "fire" : "use", Len(E::GetActorVelocity(k)), at.X, at.Y, at.Z);
    g_host = HostStep::Rest;
    g_stepMs = g_sampleMs = ms;
    g_restSince = 0;
}

void TickRest(void* k, uint64_t ms) {
    if (ms - g_sampleMs < kSampleMs) return;
    g_sampleMs = ms;
    const float speed = Len(E::GetActorVelocity(k));
    g_restSince = speed < kRestCmS ? (g_restSince ? g_restSince : ms) : 0;
    const bool rested = g_restSince && ms - g_restSince >= kRestHoldMs;
    if (!rested && ms - g_stepMs < kRestWindowMs) return;
    ue_wrap::FVector at{};
    E::TryGetActorLocation(k, at);
    if (rested)
        UE_LOGI("[KERFUS-THROW] host: throw %d rested at (%.0f,%.0f,%.0f)", g_throw, at.X, at.Y, at.Z);
    else
        UE_LOGW("[KERFUS-THROW] host: throw %d has not rested after %llu s (%.0f cm/s at (%.0f,%.0f,%.0f)); the next "
                "grab goes ahead", g_throw, static_cast<unsigned long long>(kRestWindowMs / 1000), speed, at.X, at.Y,
                at.Z);
    ++g_throw;
    g_host = HostStep::SetState;
}

void HostVerdict() {
    int moved = 0, comOff = 0;
    for (const Parked& p : g_parked) {
        if (p.riderMax > kRiderCm) ++moved;
        if (p.comMax > kComCm) ++comOff;
    }
    const bool pass = moved == 0 && comOff == 0;
    UE_LOGI("[KERFUS-THROW] host DONE %s -- of the client's %d grabs, %d moved the navigation pawn more than %.0f cm "
            "on the body and %d took the centre of mass more than %.0f cm from it", pass ? "PASS" : "FAIL",
            kClientGrabs, moved, kRiderCm, comOff, kComCm);
    g_host = HostStep::Idle;
}

// The client's grabs: its hand holds the host's Kerfus through the host's receiver, which parks it. Each hold,
// and the rest after it, is sampled; after the last release the Kerfus rests and the verdict is read.
void TickHostWatch(void* k, uint64_t ms) {
    if (ms - g_sampleMs < kSampleMs) return;
    g_sampleMs = ms;
    const bool held = coop::remote_prop::IsActorUnderAnyDrive(k);
    if (held && !g_remoteHeld) {
        if (g_remoteHolds >= kClientGrabs) { HostInvalid("the client grabbed it more than twice"); return; }
        UE_LOGI("[KERFUS-THROW] host: the client's grab %d holds my Kerfus", g_remoteHolds);
        ++g_remoteHolds;
    }
    if (!held && g_remoteHeld) {
        const Parked& p = g_parked[g_remoteHolds - 1];
        UE_LOGI("[KERFUS-THROW] host: the client's grab %d let go -- through the hold the navigation pawn moved "
                "%.1f cm on the body and the centre of mass sat at most %.1f cm from it", g_remoteHolds - 1, p.riderMax,
                p.comMax);
        g_stepMs = ms;
        g_restSince = 0;
    }
    g_remoteHeld = held;
    if (g_remoteHolds == 0) return;
    float com = 0.f, rider = 0.f;
    if (!Sample(k, g_hostShape, com, rider)) { HostInvalid("the Kerfus's shape did not read"); return; }
    Parked& p = g_parked[g_remoteHolds - 1];
    if (com > p.comMax) p.comMax = com;
    if (rider > p.riderMax) p.riderMax = rider;
    if (held || g_remoteHolds < kClientGrabs) return;
    const float speed = Len(E::GetActorVelocity(k));
    g_restSince = speed < kRestCmS ? (g_restSince ? g_restSince : ms) : 0;
    if ((g_restSince && ms - g_restSince >= kRestHoldMs) || ms - g_stepMs >= kRestWindowMs) HostVerdict();
}

void TickHost(coop::net::Session* s, uint64_t ms) {
    if (g_host == HostStep::Idle) return;
    if (g_host == HostStep::WaitJoin) {
        for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers); ++i) {
            if (!s->IsSlotWorldReady(i) || !coop::prop_snapshot::IsBracketClosed(i)) continue;
            void* k = UK::FindLive();
            if (!k) { HostInvalid("no Kerfus in the host's world"); return; }
            g_hostKerfus.Set(k);
            UE_LOGI("[KERFUS-THROW] host: slot %d's join is over -- the Kerfus is %p", i, k);
            if (!SayShape("host", k)) { HostInvalid("the Kerfus's shape did not read"); return; }
            float com = 0.f, rider = 0.f;
            Sample(k, g_hostShape, com, rider);  // the pawn's place on the body, before any throw
            g_host = HostStep::SetState;
            return;
        }
        return;
    }
    void* k = g_hostKerfus.Get();
    void* player = coop::players::Registry::Get().Local();
    if (!k || !player) { HostInvalid("the Kerfus or the host's player is gone"); return; }
    switch (g_host) {
    case HostStep::SetState: {
        bool on = false;
        if (!UK::ReadActive(k, on)) { HostInvalid("the Kerfus's `active` did not read"); return; }
        if (on == WantOn()) { StartWalkToKerfus(player, k); return; }
        if (!UK::RunActionOptionIndex(k, player, kActionToggle)) { HostInvalid("the on/off verb did not run"); return; }
        g_stepMs = ms;
        g_host = HostStep::WaitState;
        return;
    }
    case HostStep::WaitState: {
        bool on = false;
        if (UK::ReadActive(k, on) && on == WantOn()) { StartWalkToKerfus(player, k); return; }
        if (ms - g_stepMs >= kStateWindowMs) HostInvalid("the on/off verb did not change its state");
        return;
    }
    case HostStep::Place: {
        const drill_place::Result r = drill_place::Run(g_hostPlace, player, kHostSpot, ms, "the host's walk to the Kerfus");
        if (r == drill_place::Result::Pending) return;
        if (r == drill_place::Result::Failed) { HostInvalid("the host's placement before the walk did not land"); return; }
        g_hostPlaced = true;
        WalkToKerfusNow(player, k);
        return;
    }
    case HostStep::Walk: {
        const int w = g_walk ? g_walk->state.load() : 2;
        if (w == 0) return;
        if (w == 2) { HostInvalid("the walk to the Kerfus did not arrive"); return; }
        g_grab = std::make_unique<D::AimedGrab>(player, k);
        g_host = HostStep::Grab;
        return;
    }
    case HostStep::Grab: {
        const D::GrabState st = g_grab->Tick();
        if (st == D::GrabState::Working) return;
        if (st == D::GrabState::Failed) { HostInvalid(g_grab->Why()); return; }
        UE_LOGI("[KERFUS-THROW] host: grabbed it for throw %d (%s)", g_throw, WantOn() ? "on" : "off");
        PickAim(player);
        g_stepMs = ms;
        g_host = HostStep::Hold;
        return;
    }
    case HostStep::Hold:
        if (ms - g_stepMs < kHoldMs) return;
        if (g_throw < kThrows) { LetGo(player, k, ms); return; }
        // The checking grab: let go by the use key, and the host watches the client's grabs.
        if (!D::CallOnPlayer(player, L"dropGrabObject")) { HostInvalid("dropGrabObject did not run"); return; }
        UE_LOGI("[KERFUS-THROW] host: %d throws and the checking grab are done; watching the client's %d grabs",
                kThrows, kClientGrabs);
        g_sampleMs = ms;
        g_host = HostStep::Watch;
        return;
    case HostStep::Rest:
        TickRest(k, ms);
        return;
    case HostStep::Watch:
        TickHostWatch(k, ms);
        return;
    default:
        return;
    }
}

// ---- client ----

// What the client saw of one throw, from its grab to the next grab: through the park its receiver made, the
// flight and the rest.
struct Seen {
    bool on = false;
    float jump = 0.f;       // how far the next grab moved the copy
    float comMax = 0.f;     // the farthest the centre of mass sat from the body's origin
    float riderMax = 0.f;   // the farthest the navigation pawn moved on the body
    float lowestZ = 0.f;
};

enum class ClientStep { WaitQuiet, Watch, Settle, Walk, Grab, Hold, Rest, Done };
ClientStep g_client = ClientStep::WaitQuiet;
ue_wrap::CachedObjRef g_copy;
Shape g_copyShape;
Seen g_seen[kThrows + 1];
int g_grabs = 0;                // the host's grabs seen
bool g_wasHeld = false;
ue_wrap::FVector g_lastFree{};  // the copy's place at the last sample no hand held it
uint64_t g_clientSampleMs = 0, g_grabMs = 0, g_clientRestSince = 0, g_clientStepMs = 0;
bool g_snapOpen = false;        // a grab began and its snap has not been measured
int g_ownGrabs = 0;             // this client's own grabs made
std::unique_ptr<D::AimedGrab> g_clientGrab;
std::shared_ptr<D::BackgroundWalk> g_clientWalk;
drill_place::Placement g_clientPlace;   // the client's placement, once per drill, before its first walk
bool g_clientPlaced = false;

void ClientInvalid(const char* why) {
    g_client = ClientStep::Done;
    UE_LOGW("[KERFUS-THROW] INVALID (client) -- %s", why);
}

bool IsWireMirror(void* obj, const void*) {
    auto& reg = EL::Registry::Get();
    EL::Element* el = reg.Get(reg.EidForActor(obj));
    return el && el->IsMirror();
}

void SayThrow(int i) {
    const Seen& t = g_seen[i];
    UE_LOGI("[KERFUS-THROW] client: throw %d (%s) -- the next grab moved the copy %.0f cm; the centre of mass sat at "
            "most %.1f cm from the origin and the navigation pawn moved %.1f cm on the body; the copy went down to "
            "z=%.0f", i, t.on ? "on" : "off", t.jump, t.comMax, t.riderMax, t.lowestZ);
}

void ClientVerdict(uint64_t ms) {
    int strayed = 0, comOff = 0, moved = 0;
    for (int i = 0; i < kThrows; ++i) {
        if (g_seen[i].jump > kStrayCm) ++strayed;
        if (g_seen[i].comMax > kComCm) ++comOff;
        if (g_seen[i].riderMax > kRiderCm) ++moved;
    }
    const bool pass = strayed == 0 && comOff == 0 && moved == 0;
    UE_LOGI("[KERFUS-THROW] client DONE %s -- of %d throws, %d left the copy more than %.0f cm from the host's, %d took "
            "its centre of mass more than %.0f cm from its origin and %d moved its navigation pawn more than %.0f cm "
            "on the body; now my %d grabs", pass ? "PASS" : "FAIL", kThrows, strayed, kStrayCm, comOff, kComCm, moved,
            kRiderCm, kClientGrabs);
    g_clientStepMs = ms;
    g_client = ClientStep::Walk;
    g_clientWalk.reset();
}

void TickWatch(void* k, uint64_t ms) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(k, at)) { ClientInvalid("the copy's place did not read"); return; }
    const bool held = coop::remote_prop::IsActorUnderAnyDrive(k);
    if (held && !g_wasHeld) {
        // A host grab: the first movement after it is the snap to the host's hand, which ends the last throw.
        g_grabMs = ms;
        g_snapOpen = g_grabs > 0;
        if (g_grabs <= kThrows) {
            UK::ReadActive(k, g_seen[g_grabs].on);
            g_seen[g_grabs].lowestZ = at.Z;
        }
        ++g_grabs;
    }
    if (g_snapOpen) {
        const float moved = Dist(at, g_lastFree);
        if (moved > 1.f || ms - g_grabMs >= kSnapWindowMs) {
            g_snapOpen = false;
            g_seen[g_grabs - 2].jump = moved > 1.f ? moved : 0.f;
            SayThrow(g_grabs - 2);
        }
    }
    if (!held && g_wasHeld && g_grabs > kThrows) {
        g_clientStepMs = ms;
        g_clientRestSince = 0;
        g_client = ClientStep::Settle;
    }
    g_wasHeld = held;
    if (!held) g_lastFree = at;
    float com = 0.f, rider = 0.f;
    if (!Sample(k, g_copyShape, com, rider)) { ClientInvalid("the copy's shape did not read"); return; }
    if (g_grabs == 0 || g_grabs > kThrows) return;
    Seen& t = g_seen[g_grabs - 1];
    if (com > t.comMax) t.comMax = com;
    if (rider > t.riderMax) t.riderMax = rider;
    if (at.Z < t.lowestZ) t.lowestZ = at.Z;
}

// Rested below kRestCmS for kRestHoldMs, or the window since `sinceMs` is over.
bool CopyRested(void* k, uint64_t ms, uint64_t sinceMs) {
    const float speed = Len(E::GetActorVelocity(k));
    g_clientRestSince = speed < kRestCmS ? (g_clientRestSince ? g_clientRestSince : ms) : 0;
    return (g_clientRestSince && ms - g_clientRestSince >= kRestHoldMs) || ms - sinceMs >= kRestWindowMs;
}

// This client's own grabs of the Kerfus: walk to it, grab it by the use key's chain, hold it, drop it by the use
// key, let it rest; twice. The host's receiver parks its Kerfus for each hold.
void TickOwnGrabs(void* k, uint64_t ms) {
    void* player = coop::players::Registry::Get().Local();
    if (!player) { ClientInvalid("this client's player is gone"); return; }
    switch (g_client) {
    case ClientStep::Walk: {
        if (!g_clientPlaced) {
            const drill_place::Result r = drill_place::Run(g_clientPlace, player, kClientSpot, ms, "the client's walk to the Kerfus");
            if (r == drill_place::Result::Pending) return;
            if (r == drill_place::Result::Failed) { ClientInvalid("the client's placement before the walk did not land"); return; }
            g_clientPlaced = true;
        }
        if (!g_clientWalk) {
            ue_wrap::FVector at{};
            if (!E::TryGetActorLocation(k, at)) { ClientInvalid("the copy's place did not read"); return; }
            g_clientWalk = WalkTo(player, at);
            return;
        }
        const int w = g_clientWalk->state.load();
        if (w == 0) return;
        if (w == 2) { ClientInvalid("the walk to the Kerfus did not arrive"); return; }
        g_clientGrab = std::make_unique<D::AimedGrab>(player, k);
        g_client = ClientStep::Grab;
        return;
    }
    case ClientStep::Grab: {
        const D::GrabState st = g_clientGrab->Tick();
        if (st == D::GrabState::Working) return;
        if (st == D::GrabState::Failed) { ClientInvalid(g_clientGrab->Why()); return; }
        UE_LOGI("[KERFUS-THROW] client: my grab %d holds the Kerfus", g_ownGrabs);
        g_clientStepMs = ms;
        g_client = ClientStep::Hold;
        return;
    }
    case ClientStep::Hold:
        if (ms - g_clientStepMs < kHoldMs) return;
        if (!D::CallOnPlayer(player, L"dropGrabObject")) { ClientInvalid("dropGrabObject did not run"); return; }
        UE_LOGI("[KERFUS-THROW] client: my grab %d let go", g_ownGrabs);
        g_clientStepMs = ms;
        g_clientRestSince = 0;
        g_client = ClientStep::Rest;
        return;
    case ClientStep::Rest:
        if (!CopyRested(k, ms, g_clientStepMs)) return;
        if (++g_ownGrabs < kClientGrabs) {
            g_clientWalk.reset();
            g_client = ClientStep::Walk;
            return;
        }
        UE_LOGI("[KERFUS-THROW] client: my %d grabs are done", kClientGrabs);
        g_client = ClientStep::Done;
        return;
    default:
        return;
    }
}

void TickClient() {
    const uint64_t ms = ::GetTickCount64();
    if (g_client == ClientStep::Done) return;
    if (g_client == ClientStep::WaitQuiet) {
        if (!coop::join_membership_sweep::HasLoadTailQuiesced()) return;
        void* k = UK::FindLive(&IsWireMirror, nullptr);
        if (!k) { ClientInvalid("no Kerfus mirror once the join's load tail quiesced"); return; }
        g_copy.Set(k);
        UE_LOGI("[KERFUS-THROW] client: watching the Kerfus mirror %p", k);
        if (!SayShape("client", k)) { ClientInvalid("the copy's shape did not read"); return; }
        g_client = ClientStep::Watch;
        return;
    }
    void* k = g_copy.Get();
    if (!k) { ClientInvalid("the Kerfus mirror is gone"); return; }
    if (ms - g_clientSampleMs < kSampleMs) return;
    g_clientSampleMs = ms;
    if (g_client == ClientStep::Watch) { TickWatch(k, ms); return; }
    if (g_client == ClientStep::Settle) {
        // After the checking grab's release: the copy rests, or the window ends, and the verdict is read.
        if (CopyRested(k, ms, g_clientStepMs)) ClientVerdict(ms);
        return;
    }
    TickOwnGrabs(k, ms);
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!IsEnabled_()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled_()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_saidArm) {
        g_saidArm = true;
        UE_LOGI("[KERFUS-THROW] %s armed", s->role() == coop::net::Role::Host ? "host" : "client");
    }
    if (s->role() == coop::net::Role::Host) TickHost(s, ::GetTickCount64());
    else if (s->connected()) TickClient();
}

void OnDisconnect() {
    g_saidArm = false;
    g_host = HostStep::WaitJoin;
    g_throw = 0;
    g_hostKerfus.Reset();
    g_grab.reset();
    g_walk.reset();
    g_aim = ue_wrap::FRotator{};
    g_stepMs = g_sampleMs = g_restSince = 0;
    g_hostShape = Shape{};
    for (Parked& p : g_parked) p = Parked{};
    g_remoteHolds = 0;
    g_remoteHeld = false;
    g_client = ClientStep::WaitQuiet;
    g_copy.Reset();
    g_copyShape = Shape{};
    for (Seen& t : g_seen) t = Seen{};
    g_grabs = 0;
    g_hostPlace = drill_place::Placement{};
    g_hostPlaced = false;
    g_clientPlace = drill_place::Placement{};
    g_clientPlaced = false;
    g_wasHeld = false;
    g_lastFree = ue_wrap::FVector{};
    g_clientSampleMs = g_grabMs = g_clientRestSince = g_clientStepMs = 0;
    g_snapOpen = false;
    g_ownGrabs = 0;
    g_clientGrab.reset();
    g_clientWalk.reset();
}

}  // namespace coop::dev::kerfus_throw_drill
