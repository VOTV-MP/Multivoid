// coop/dev/fireext_drill.cpp -- see coop/dev/fireext_drill.h.

#include "coop/dev/fireext_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/net/session.h"
#include "coop/player/local_streams.h"  // CurrentHoldGen: the hold the stale tail names
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/remote_prop.h"  // IsActorUnderAnyDrive: the re-latch probe
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::fireext_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace FX = ue_wrap::fire_extinguisher;
namespace PR = ue_wrap::prop;

// carry: the host takes it off and carries it off. short: the host lets go the moment it holds it.
// client: the client carries, the host watches. join: the host takes it off while a joiner loads.
enum class Arm { Off, Carry, Short, Client, Join };
enum class Step { WaitJoin, WalkTo, Aim, Grab, WalkAway, Rest, Done, Invalid };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::fireext_drill);
        return v == "carry" ? Arm::Carry : v == "short" ? Arm::Short : v == "client" ? Arm::Client
             : v == "join" ? Arm::Join : Arm::Off;
    }();
    return a;
}

// The peer that grabs: the client in the client arm, the host otherwise.
bool LocalActs() { return (ArmOf() == Arm::Client) != coop::roster::LocalIsHost(); }
char Who() { return coop::roster::LocalIsHost() ? 'H' : 'C'; }

constexpr float kReachCm        = 110.f;  // the director's stop distance from the extinguisher, level
constexpr float kCarryCm        = 450.f;  // how far off the carry's end should be
constexpr int   kAimTicksPerPose = 6;     // the trace runs on the player's own tick: hold each pose
constexpr int   kAimFanHalf     = 5;      // an 11 x 11 fan of 3 degrees round the extinguisher
constexpr float kAimFanStepDeg  = 3.f;
constexpr int   kGrabVerifyTicks = 30;
constexpr int   kRestMaxTicks   = 600;    // ~10 s for a dropped extinguisher to come to rest
// A walk that stops making way ends here. The rig save puts the client about 760 m from the base,
// a route of some 200 s (the container probe's long-route deadline).
constexpr int   kWalkDeadlineS  = 300;
constexpr int   kWatchEveryTicks = 15;
constexpr float kWatchMoveCm    = 3.f;
constexpr float kRouteEndReachCm = 160.f; // a route that ends further from the wall reaches no mount
constexpr float kProbeOffCm     = 50.f;   // the carried copy is off its mount before the probe runs
constexpr int   kProbeReadTicks = 3;      // the drive's tick has run between the verb and this read
constexpr int   kStaleTailTicks = 30;     // the short arm's late poses of the closed hold

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- The watch: both peers, every extinguisher --------------------------------------------------

struct Watched {
    ue_wrap::CachedObjRef ref;
    std::wstring key;
    ue_wrap::FVector start{}, printed{};
    bool frozen = false, mounted = false, gone = false;
    bool unread = false;              // its location could not be read: said once, and no reader reads it again
    int thrusting = 0, spraying = 0;  // a drop hard enough starts the runaway thrust; -1 = unread
};
std::vector<Watched> g_watched;
std::vector<ue_wrap::CachedObjRef> g_mounts;
bool g_watchArmed = false;
int  g_watchTick = 0;

// What the mounts hold, read once per watch pass rather than once per extinguisher.
std::vector<void*> g_held;

void ReadMounts() {
    g_held.clear();
    for (const auto& m : g_mounts)
        if (void* mount = m.Get())
            if (void* ext = FX::MountedExtinguisher(mount)) g_held.push_back(ext);
}

bool IsMounted(void* ext) {
    return std::find(g_held.begin(), g_held.end(), ext) != g_held.end();
}

// Silence reads as "did not move", so the first reader that cannot place an extinguisher says so, and
// no reader reads it again: a failed read of a live actor is a faulted dispatch, which faults again.
void MarkUnread(Watched& w, char who, const char* reader) {
    w.unread = true;
    UE_LOGW("[FIREEXT-DRILL] [%c] UNREAD key='%ls' -- %s could not read its location; the watch cannot say "
            "whether it moved, and nothing reads it again", who, w.key.c_str(), reader);
}

// The watch's row for `o`, by the actor itself; null when the watch has none.
Watched* RowOf(void* o) {
    if (!o) return nullptr;
    for (auto& w : g_watched)
        if (w.ref.Raw() == o) return &w;
    return nullptr;
}

// Every reader of a watched extinguisher's place goes through here: a latched row is not read, and a read
// that fails latches its row. False when there is no place.
bool ReadWatched(void* o, ue_wrap::FVector& at, char who, const char* reader) {
    Watched* row = RowOf(o);
    if (row && row->unread) return false;
    if (E::TryGetActorLocation(o, at)) return true;
    if (row) MarkUnread(*row, who, reader);
    return false;
}

// One walk of the object array, when the watch arms, never again.
void ArmWatch(char who) {
    g_watched.clear();
    g_mounts.clear();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        // The class tests read name indices; the name render the CDO test needs runs on a match only.
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        const bool mount = FX::IsMount(o);
        if (!mount && !FX::IsExtinguisher(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        if (mount) {
            g_mounts.emplace_back();
            g_mounts.back().Set(o);
        } else {
            Watched w;
            w.ref.Set(o);
            w.key = PR::GetInteractableKeyString(o);
            if (!E::TryGetActorLocation(o, w.start)) {
                UE_LOGW("[FIREEXT-DRILL] [%c] NOT WATCHED key='%ls' -- its location could not be read at the arm",
                        who, w.key.c_str());
                continue;
            }
            w.printed = w.start;
            w.frozen = PR::IsFrozen(o);
            g_watched.push_back(std::move(w));
        }
    }
    ReadMounts();
    for (auto& w : g_watched) {
        void* o = w.ref.Get();
        w.mounted = o && IsMounted(o);
        float charge = -1.f;
        if (o) FX::ReadCharge(o, charge);
        UE_LOGI("[FIREEXT-DRILL] [%c] HAS key='%ls' at (%.1f, %.1f, %.1f) frozen=%d mounted=%d charge=%.2f",
                who, w.key.c_str(), w.start.X, w.start.Y, w.start.Z, w.frozen ? 1 : 0, w.mounted ? 1 : 0,
                charge);
    }
    UE_LOGI("[FIREEXT-DRILL] [%c] watching %zu extinguishers and %zu mounts", who, g_watched.size(),
            g_mounts.size());
    g_watchArmed = true;
}

void TickWatch(char who) {
    if (!g_watchArmed || ++g_watchTick < kWatchEveryTicks) return;
    g_watchTick = 0;
    ReadMounts();
    for (auto& w : g_watched) {
        if (w.gone || w.unread) continue;
        void* o = w.ref.Get();
        if (!o) {
            w.gone = true;
            UE_LOGI("[FIREEXT-DRILL] [%c] GONE key='%ls'", who, w.key.c_str());
            continue;
        }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(o, at)) {
            MarkUnread(w, who, "the watch");
            continue;
        }
        const bool frozen = PR::IsFrozen(o);
        const bool mounted = IsMounted(o);
        bool th = false, sp = false;
        const int thrusting = FX::ReadThrusting(o, th) ? (th ? 1 : 0) : -1;
        const int spraying = FX::ReadSpraying(o, sp) ? (sp ? 1 : 0) : -1;
        if (Dist(at, w.printed) < kWatchMoveCm && frozen == w.frozen && mounted == w.mounted &&
            thrusting == w.thrusting && spraying == w.spraying)
            continue;
        w.printed = at;
        w.frozen = frozen;
        w.mounted = mounted;
        w.thrusting = thrusting;
        w.spraying = spraying;
        UE_LOGI("[FIREEXT-DRILL] [%c] key='%ls' at (%.1f, %.1f, %.1f) fromStart=%.1fcm frozen=%d mounted=%d "
                "thrusting=%d spraying=%d", who, w.key.c_str(), at.X, at.Y, at.Z, Dist(at, w.start),
                frozen ? 1 : 0, mounted ? 1 : 0, thrusting, spraying);
    }
}

// ---- The re-latch probe: the carry arm's watching client ------------------------------------------

// Once its copy of an extinguisher is under the host's drive and off the mount, the client runs the
// verb its own laptop exit runs on a chair another peer carries: setPropProps(F,F,F,F), whose init()
// switches simulation on. The drive re-latches the copy kinematic; the probe reads the simulate
// state right after the verb and again a few ticks later, and the watch shows whether the copy kept
// following the carry.
ue_wrap::CachedObjRef g_probed;
std::wstring g_probedKey;
int g_probeTicks = -1;  // -1 = not yet run

void TickRelatchProbe() {
    if (g_probeTicks < 0) {
        for (auto& w : g_watched) {
            if (w.gone || w.unread) continue;
            void* o = w.ref.Get();
            if (!o || !coop::remote_prop::IsActorUnderAnyDrive(o)) continue;
            ue_wrap::FVector at{};
            if (!E::TryGetActorLocation(o, at)) {
                MarkUnread(w, 'C', "the re-latch probe");
                continue;
            }
            if (Dist(at, w.start) < kProbeOffCm) continue;
            const bool ok = PR::CallSetPropProps(o, false, false, false);
            const bool sim = ue_wrap::engine::IsComponentSimulatingPhysics(PR::GetStaticMesh(o));
            UE_LOGI("[FIREEXT-DRILL] [C] RELATCH PROBE key='%ls' setPropProps(F,F,F,F) on the driven copy (%s) "
                    "-- simulating=%d", w.key.c_str(), ok ? "dispatched" : "did not dispatch", sim ? 1 : 0);
            g_probed.Set(o);
            g_probedKey = w.key;
            g_probeTicks = 0;
            return;
        }
        return;
    }
    if (g_probeTicks >= kProbeReadTicks || ++g_probeTicks < kProbeReadTicks) return;
    void* o = g_probed.Get();
    UE_LOGI("[FIREEXT-DRILL] [C] RELATCH PROBE key='%ls' %d ticks later -- simulating=%d underDrive=%d",
            g_probedKey.c_str(), kProbeReadTicks,
            o ? (ue_wrap::engine::IsComponentSimulatingPhysics(PR::GetStaticMesh(o)) ? 1 : 0) : -1,
            o ? (coop::remote_prop::IsActorUnderAnyDrive(o) ? 1 : 0) : -1);
}

// ---- The release readout: the carry arm's watching client, after the drive ends -----------------

// Once the probed copy's drive has ended, what its body does: its place, whether it simulates,
// whether it is at rest, its flags and its velocity, a few times over the following second.
int g_afterTicks = -1;  // -1 = the drive has not ended yet

void TickReleaseReadout() {
    void* o = g_probed.Get();
    if (!o || g_probeTicks < kProbeReadTicks || g_afterTicks > 60) return;
    if (g_afterTicks < 0) {
        if (coop::remote_prop::IsActorUnderAnyDrive(o)) return;
        g_afterTicks = 0;
    } else {
        ++g_afterTicks;
    }
    const int t = g_afterTicks;
    if (t != 0 && t != 1 && t != 2 && t != 5 && t != 10 && t != 30 && t != 60) return;
    ue_wrap::FVector at{};
    const bool atRead = ReadWatched(o, at, 'C', "the release readout");
    const PR::VelocityState v = PR::GetPhysicsVelocity(o);
    char place[64] = "(unread)";   // no numbers: an unread place is no place
    if (atRead) std::snprintf(place, sizeof(place), "(%.1f, %.1f, %.1f)", at.X, at.Y, at.Z);
    UE_LOGI("[FIREEXT-DRILL] [C] AFTER RELEASE +%d ticks key='%ls' at %s simulating=%d atRest=%d frozen=%d sleep=%d "
            "vel=(%.1f, %.1f, %.1f)", t, g_probedKey.c_str(), place,
            ue_wrap::engine::IsComponentSimulatingPhysics(PR::GetStaticMesh(o)) ? 1 : 0,
            E::IsActorRootBodyAtRest(o) ? 1 : 0, PR::IsFrozen(o) ? 1 : 0, PR::IsSleeping(o) ? 1 : 0,
            v.linearCmS.X, v.linearCmS.Y, v.linearCmS.Z);
}

// ---- The acting peer's steps -------------------------------------------------------------------

Step g_step = Step::WaitJoin;
ue_wrap::CachedObjRef g_target;
std::wstring g_targetKey;
ue_wrap::FVector g_mountPos{};
int g_stepTicks = 0;
int g_aimPose = 0;

// ---- The stale tail: the short arm's second measurement, on the host ----------------------------

// For a moment after the drop the host sends its last pose again under the hold its release just
// closed: the tail a slow network delivers after a release, made long and certain. Every receiver
// must drop it (the hold is closed) and leave its copy to its own physics; the watch shows whether
// one was pulled back. The pose stays set in the session until cleared, so every way out of the
// tail clears it: its end, a target that died, a tick on which the drill does not act.
uint16_t g_staleGen = 0;
bool g_tailSet = false;
bool g_tailEnded = false;   // the tail stopped early and said why: no later tick sends

void ClearStaleTail(coop::net::Session& s) {
    if (!g_tailSet) return;
    s.SetLocalPropPose(false, {});
    g_tailSet = false;
}

void SendStaleTail(coop::net::Session& s, void* t, int tick) {
    if (tick > kStaleTailTicks + 1 || g_tailEnded) return;
    if (tick > kStaleTailTicks) {
        ClearStaleTail(s);
        return;
    }
    // Read before the tail is announced: a tail that sends nothing would make the receivers' "not
    // pulled back" pass by default. The pose stays set between ticks, so a cut tail held it for the
    // ticks before the cut; how many packets carried it is the net thread's cadence, not this count.
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    const char* lost = !t ? " is gone"
                     : (!ReadWatched(t, loc, Who(), "the stale tail") || !E::TryGetActorRotation(t, rot))
                           ? "'s location or rotation could not be read"
                           : nullptr;
    if (lost) {
        g_tailEnded = true;
        ClearStaleTail(s);
        if (tick == 1)
            UE_LOGW("[FIREEXT-DRILL] [%c] STALE TAIL INVALID -- the extinguisher%s before the tail began, so no "
                    "stale pose is sent and this arm's second measurement has no stimulus", Who(), lost);
        else
            UE_LOGW("[FIREEXT-DRILL] [%c] STALE TAIL CUT at tick %d of %d -- the extinguisher%s; the stale pose "
                    "was held for %d tick(s)", Who(), tick, kStaleTailTicks, lost, tick - 1);
        return;
    }
    if (tick == 1) {
        g_staleGen = coop::local_streams::CurrentHoldGen();
        UE_LOGI("[FIREEXT-DRILL] [%c] STALE TAIL: re-sending hold %u's pose after its release, %d ticks",
                Who(), static_cast<unsigned>(g_staleGen), kStaleTailTicks);
    }
    coop::net::PropPoseSnapshot pp{};
    for (size_t i = 0; i < g_targetKey.size() && pp.key.len < 31; ++i)
        pp.key.data[pp.key.len++] = static_cast<char>(g_targetKey[i]);
    pp.holdGen = g_staleGen;
    pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
    pp.pitch = rot.Pitch; pp.yaw = rot.Yaw; pp.roll = rot.Roll;
    s.SetLocalPropPose(true, pp);
    g_tailSet = true;
}

// The aim fan, nearest pose first: the extinguisher's origin, then offsets of growing size.
const std::vector<std::pair<int, int>>& AimFan() {
    static const std::vector<std::pair<int, int>> fan = [] {
        std::vector<std::pair<int, int>> v;
        for (int p = -kAimFanHalf; p <= kAimFanHalf; ++p)
            for (int y = -kAimFanHalf; y <= kAimFanHalf; ++y) v.emplace_back(p, y);
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
        });
        return v;
    }();
    return fan;
}

// The director blocks, so a walk runs on a worker; the step polls the shared state.
struct Walk {
    coop::director::DirectorGoal goal;
    bool carry = false;          // the hand keeps what it holds
    std::atomic<int> state{0};   // 0 walking, 1 reached, 2 failed
};
std::shared_ptr<Walk> g_walk;

DWORD WINAPI WalkThread(LPVOID arg) {
    auto* holder = static_cast<std::shared_ptr<Walk>*>(arg);
    std::shared_ptr<Walk> w = *holder;
    delete holder;
    coop::director::ControlManager mgr;
    if (w->carry) coop::director::AddCarryToProcesses(mgr, w->goal);
    else coop::director::AddWalkToProcesses(mgr, w->goal);
    mgr.Run(w->goal, kWalkDeadlineS);
    w->state.store(w->goal.reached ? 1 : 2);
    return 0;
}

void StartWalk(const ue_wrap::FVector& to, float reachCm, bool carry) {
    g_walk = std::make_shared<Walk>();
    g_walk->goal.targetPos = to;
    g_walk->goal.reachCm = reachCm;
    g_walk->carry = carry;
    auto* arg = new std::shared_ptr<Walk>(g_walk);
    if (HANDLE t = ::CreateThread(nullptr, 0, &WalkThread, arg, 0, nullptr)) ::CloseHandle(t);
    else { delete arg; g_walk->state.store(2); }
}

// 0 walking, 1 reached, 2 failed.
int WalkState() { return g_walk ? g_walk->state.load() : 2; }

// The run's dead marker: a run passes --dead-marker "[FIREEXT-DRILL] INVALID", which ends it
// INCONCLUSIVE, so a drill that measured nothing never ends on the done marker a measurement prints.
void Invalid(const char* why) {
    UE_LOGW("[FIREEXT-DRILL] INVALID %s", why);
    g_step = Step::Invalid;
}

void Go(Step s) {
    g_step = s;
    g_stepTicks = 0;
}

bool CallWithPlayer(void* obj, const wchar_t* fnName, void* player) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(obj, f);
}

bool CallOnPlayer(void* player, const wchar_t* fnName) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(player), fnName);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(player, f);
}

void* Grabbing(void* player) {
    E::MainPlayerGrabState gs{};
    return E::ReadMainPlayerGrabState(player, gs) ? gs.grabbingActor : nullptr;
}

// The mounted extinguisher at the end of the shortest NavMesh route from the player, from the
// watch's own lists: a route must exist and end within reach of the wall, as the director's pile
// pick asks (director_run.cpp, PickReachablePile).
bool PickTarget(void* player, const char*& why) {
    ue_wrap::FVector me{};
    if (!E::TryGetActorLocation(player, me)) { why = "the player's location could not be read"; return false; }
    why = "no mounted, frozen extinguisher a route reaches";
    float best = 1e30f;
    ue_wrap::FVector bestAt{};
    int unread = 0;
    for (auto& w : g_watched) {
        void* o = w.ref.Get();
        if (!o || !w.mounted || !PR::IsFrozen(o)) continue;
        if (w.unread) { ++unread; continue; }   // a candidate with no place: never the target
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(o, at)) {
            MarkUnread(w, Who(), "the target pick");
            ++unread;
            continue;
        }
        std::vector<ue_wrap::FVector> route;
        if (!E::FindNavPath(player, me, at, route) || route.empty()) continue;
        const ue_wrap::FVector& end = route.back();
        if (std::hypot(end.X - at.X, end.Y - at.Y) > kRouteEndReachCm) continue;
        float len = 0.f;
        for (size_t i = 1; i < route.size(); ++i) len += Dist(route[i - 1], route[i]);
        if (len < best) { best = len; g_target.Set(o); g_targetKey = w.key; bestAt = at; }
    }
    if (unread > 0)
        UE_LOGW("[FIREEXT-DRILL] [%c] PICK -- %d extinguisher(s) could not be placed, so none of them is a candidate",
                Who(), unread);
    if (!g_target.Raw()) return false;
    g_mountPos = bestAt;   // read in the pick above
    UE_LOGI("[FIREEXT-DRILL] [%c] target key='%ls' at (%.1f, %.1f, %.1f), a %.0f cm route (%.0f cm straight)",
            Who(), g_targetKey.c_str(), g_mountPos.X, g_mountPos.Y, g_mountPos.Z, best, Dist(g_mountPos, me));
    return true;
}

// A point the NavMesh routes to, some metres from the mount: the route's own last point is on the
// mesh by construction, which a computed offset is not.
bool PickCarryEnd(void* player, ue_wrap::FVector& out, const char*& why) {
    ue_wrap::FVector me{};
    if (!E::TryGetActorLocation(player, me)) { why = "the player's location could not be read"; return false; }
    why = "no NavMesh route to carry it along";
    for (int k = 0; k < 8; ++k) {
        const float a = static_cast<float>(k) * 0.785398f;
        const ue_wrap::FVector want{me.X + kCarryCm * std::cos(a), me.Y + kCarryCm * std::sin(a), me.Z};
        std::vector<ue_wrap::FVector> route;
        if (!E::FindNavPath(player, me, want, route) || route.size() < 2) continue;
        if (Dist(route.back(), g_mountPos) < kCarryCm * 0.6f) continue;
        out = route.back();
        return true;
    }
    return false;
}

void LogTarget(const char* what) {
    ReadMounts();
    void* t = g_target.Get();
    if (!t) { UE_LOGI("[FIREEXT-DRILL] [%c] %s key='%ls' -- the extinguisher is gone", Who(), what, g_targetKey.c_str()); return; }
    ue_wrap::FVector at{};
    const bool atRead = ReadWatched(t, at, Who(), "the target log");
    if (atRead)
        UE_LOGI("[FIREEXT-DRILL] [%c] %s key='%ls' at (%.1f, %.1f, %.1f) fromMount=%.1fcm frozen=%d mounted=%d",
                Who(), what, g_targetKey.c_str(), at.X, at.Y, at.Z, Dist(at, g_mountPos), PR::IsFrozen(t) ? 1 : 0,
                IsMounted(t) ? 1 : 0);
    else   // no numbers: an unread place is no place
        UE_LOGI("[FIREEXT-DRILL] [%c] %s key='%ls' at (unread) frozen=%d mounted=%d", Who(), what,
                g_targetKey.c_str(), PR::IsFrozen(t) ? 1 : 0, IsMounted(t) ? 1 : 0);
}

// When the acting peer may start. carry and short: a client's join and its join window are over,
// since while the host's late flush is armed a move reaches the joiner as the join's position
// correction rather than through the lane under test. join: the opposite, a joiner whose world was
// captured and has not come up, so the whole move lands inside its load. client: the client's own
// join is over.
bool ActorMayStart(coop::net::Session& s) {
    if (ArmOf() == Arm::Client)
        return coop::net_pump::HasAnnouncedWorldReady() &&
               coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        if (ArmOf() == Arm::Join) {
            if (coop::join_window_baseline::HasCapture(slot) && !s.IsSlotWorldReady(slot)) return true;
        } else if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
                   !coop::join_window_baseline::IsLateWindowOpen(slot)) {
            return true;
        }
    }
    return false;
}

void ActStep(coop::net::Session& s, void* player) {
    ++g_stepTicks;
    switch (g_step) {
    case Step::WaitJoin: {
        if (!ActorMayStart(s)) return;
        if (!g_watchArmed) ArmWatch(Who());
        const char* why = "";
        if (!PickTarget(player, why)) { Invalid(why); return; }
        StartWalk(g_mountPos, kReachCm, /*carry=*/false);
        Go(Step::WalkTo);
        return;
    }
    case Step::WalkTo:
        if (WalkState() == 0) return;
        if (WalkState() == 2) { Invalid("the walk to the extinguisher did not arrive"); return; }
        g_aimPose = 0;
        Go(Step::Aim);
        return;
    case Step::Aim: {
        void* t = g_target.Get();
        if (!t) { Invalid("the extinguisher died before the grab"); return; }
        if (E::ReadMainPlayerHitActor(player) == t) {
            UE_LOGI("[FIREEXT-DRILL] [%c] the trace took the extinguisher at fan pose %d", Who(), g_aimPose);
            Go(Step::Grab);
            return;
        }
        if (g_stepTicks % kAimTicksPerPose != 1) return;
        const auto& fan = AimFan();
        if (g_aimPose >= static_cast<int>(fan.size())) {
            void* hit = E::ReadMainPlayerHitActor(player);
            UE_LOGI("[FIREEXT-DRILL] [%c] the fan ended on '%ls'", Who(), hit ? R::ClassNameOf(hit).c_str() : L"nothing");
            Invalid("no aim the trace would take");
            return;
        }
        // The bounds' centre: a lying extinguisher's origin can sit at the floor's surface.
        ue_wrap::FVector centre{}, extent{};
        if (!E::GetActorBounds(t, /*onlyColliding=*/true, centre, extent) && !ReadWatched(t, centre, Who(), "the aim")) {
            Invalid("the extinguisher has neither bounds nor a readable location to aim at");
            return;
        }
        ue_wrap::FRotator r = coop::director::LookAt(E::GetCameraLocation(), centre);
        r.Pitch += kAimFanStepDeg * static_cast<float>(fan[g_aimPose].first);
        r.Yaw   += kAimFanStepDeg * static_cast<float>(fan[g_aimPose].second);
        E::SetControlRotation(E::GetController(player), r);
        ++g_aimPose;
        return;
    }
    case Step::Grab: {
        void* t = g_target.Get();
        if (!t) { Invalid("the extinguisher died before the grab"); return; }
        if (g_stepTicks == 1) {
            LogTarget("BEFORE GRAB");
            // The use key's release on an aimed prop with an empty hand, in its order.
            const bool pre = CallWithPlayer(t, L"playerGrabbed_pre", player);
            void* useFn = R::FindDispatchFunctionCached(R::ClassOf(player), L"useAction");
            bool use = false;
            if (useFn) {
                ue_wrap::ParamFrame f(useFn);
                use = f.valid() && f.Set<bool>(L"sec", false) && ue_wrap::Call(player, f);
            }
            const bool post = CallWithPlayer(t, L"playerGrabbed", player);
            UE_LOGI("[FIREEXT-DRILL] [%c] grab chain: playerGrabbed_pre=%d useAction=%d playerGrabbed=%d",
                    Who(), pre ? 1 : 0, use ? 1 : 0, post ? 1 : 0);
            return;
        }
        if (Grabbing(player) == t) {
            LogTarget("GRABBED");
            if (ArmOf() == Arm::Short || ArmOf() == Arm::Join) {
                // Let go at once: the stream carries a pose or two, then the release.
                CallOnPlayer(player, L"dropGrabObject");
                LogTarget("DROPPED");
                Go(Step::Rest);
                return;
            }
            ue_wrap::FVector end{};
            const char* why = "";
            if (!PickCarryEnd(player, end, why)) {
                CallOnPlayer(player, L"dropGrabObject");   // the drill ends with an empty hand
                LogTarget("DROPPED");
                Invalid(why);
                return;
            }
            StartWalk(end, 80.f, /*carry=*/true);
            Go(Step::WalkAway);
            return;
        }
        if (g_stepTicks > kGrabVerifyTicks) Invalid("the grab chain did not put the extinguisher in the hand");
        return;
    }
    case Step::WalkAway:
        if (g_stepTicks % 30 == 0) LogTarget("CARRY");
        if (WalkState() == 0) return;
        if (Grabbing(player) != g_target.Get()) { Invalid("the hand lost the extinguisher on the way"); return; }
        LogTarget("CARRIED");
        CallOnPlayer(player, L"dropGrabObject");
        LogTarget("DROPPED");
        Go(Step::Rest);
        return;
    case Step::Rest: {
        void* t = g_target.Get();
        if (ArmOf() == Arm::Short) SendStaleTail(s, t, g_stepTicks);
        const bool rested = t && g_stepTicks > 30 && E::IsActorRootBodyAtRest(t);
        if (!rested && g_stepTicks < kRestMaxTicks) return;
        ClearStaleTail(s);  // the tail ends here whatever its length
        LogTarget(rested ? "RESTED" : "NOT AT REST after the wait");
        Go(Step::Done);
        return;
    }
    case Step::Done:
        UE_LOGI("[FIREEXT-DRILL] ACTOR DONE");
        g_step = Step::Invalid;  // terminal: nothing further runs, and nothing is printed twice
        g_walk.reset();
        return;
    case Step::Invalid:
        return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected()) return;
    if (!FX::ResolveNames()) return;
    if (LocalActs()) {
        if (g_step != Step::Invalid) {
            void* player = coop::players::Registry::Get().Local();
            if (!player || !R::IsLive(player) || !E::GetController(player)) {
                ClearStaleTail(*session);  // no step runs to end it
                return;
            }
            ActStep(*session, player);
        }
        TickWatch(Who());
        return;
    }
    // The watching peer arms at its own join's end, or at once on a host (its world is up).
    if (!g_watchArmed) {
        if (!coop::roster::LocalIsHost() &&
            (!coop::net_pump::HasAnnouncedWorldReady() ||
             coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle))
            return;
        ArmWatch(Who());
    }
    TickWatch(Who());
    if (ArmOf() == Arm::Carry) {
        TickRelatchProbe();
        TickReleaseReadout();
    }
}

void OnDisconnect() {
    g_tailSet = false;  // the session drops its own local pose at the end
    g_watched.clear();
    g_mounts.clear();
    g_watchArmed = false;
    g_watchTick = 0;
    g_step = Step::WaitJoin;
    g_stepTicks = 0;
    g_target.Reset();
    g_targetKey.clear();
    g_held.clear();
    g_probed.Reset();
    g_probedKey.clear();
    g_probeTicks = -1;
    g_afterTicks = -1;
    g_tailEnded = false;
    if (g_walk) {  // the worker still holds it: the director's run ends at its next tick
        g_walk->goal.failed = true;
        g_walk->goal.failReason = "session ended";
    }
    g_walk.reset();
}

}  // namespace coop::dev::fireext_drill
