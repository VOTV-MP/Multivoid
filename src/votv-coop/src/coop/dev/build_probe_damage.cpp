// coop/dev/build_probe_damage.cpp -- the host's legs at a wall: a thrown brick, a swung axe, an explosion beside it, first
// at the level's wall and then at a customWall_C the probe placed on the ground beside the host; see coop/dev/build_probe.h.
// The director walks the host to the wall, never a teleport. A leg that destroys the wall ends that wall's legs: what fired
// is the result, and nothing is left to hit. Each throw says how near the brick came to the wall's box and each swing says
// what its own view trace struck, so a leg with no route can be told from a miss. Before a throw or a swing the view
// trace must name the wall; where another actor blocks it the host walks to another side of the wall's box (up to four),
// and a leg that finds no side with a clear view says what blocks it and moves on.

#include "build_probe_detail.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/input_stub.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"

#include "drive_drill_verbs.h"  // Throw: the hand's throw (co-located private header)

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/world/explosion.h"
#include "ue_wrap/world/weapon_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::build_probe::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DR = coop::director;
namespace DV = coop::dev::drive_drill_verbs;

enum class Step : uint8_t { Start, Walk, Restand, ThrowAim, ThrowSettle, Swing, Explode, ExplodeWait, Place, End };

// What a look at the wall found: it is the first thing the view trace strikes, the host is walking to another stand to
// find a line that names it, or every stand was tried.
enum class Sight : uint8_t { Named, Walking, Blocked };

constexpr const wchar_t* kBuiltPath = L"/Game/objects/customWall.customWall_C";
constexpr float    kBuiltAwayCm = 250.f;   // the host-built wall is put this far from the host, away from the level wall
constexpr float    kReachCm = 150.f;       // the walk ends this near the wall: a swing's reach
constexpr int      kWalkMinS = 90;
constexpr int      kWalkMaxS = 300;
constexpr uint64_t kAimBoundMs = 10000;    // the camera follows the control rotation within this
constexpr uint64_t kAimHoldMs = 300;       // a throw's camera follows the control rotation on the next frames
constexpr uint64_t kSettleBoundMs = 30000; // a thrown brick rests or is gone within this
constexpr int      kMaxSwings = 6;
constexpr uint64_t kSwingGapMs = 600;      // a player's swing rate
constexpr int      kRestTicks = 3;
constexpr float    kRestCmS = 10.f;
constexpr float    kMovingCmS = 50.f;
constexpr float    kBlastDamage = 40.f;
constexpr float    kBlastRadiusCm = 400.f;
constexpr float    kBlastStandoffCm = 120.f;
constexpr float    kDegToRad = 0.01745329252f;
constexpr float    kThrowTraceCm = 600.f;   // a throw's view trace runs this far
constexpr float    kStandOffCm = 90.f;      // a stand is this far off a face of the wall's box
constexpr float    kStandReachCm = 60.f;    // the walk to a stand ends this near it
constexpr size_t   kMaxStands = 4;          // the sides of the box, each tried once per leg

// The leg names per pass: the level's wall, then the host-built one.
constexpr const char* kLegs[2][3] = {{"throw", "swing", "explosion"}, {"built-throw", "built-swing", "built-explosion"}};
constexpr const char* kLabels[2] = {"level", "host-built"};

Step     g_step = Step::Start;
int      g_pass = 0;
uint64_t g_stepMs = 0;
uint64_t g_nextSwingMs = 0;
int      g_swings = 0;
float    g_swingReachCm = 0.f;
float    g_healthBefore = 0.f;
bool     g_inLeg = false;
const char* g_leg = "";
bool     g_thrownSeen = false;
int      g_restReads = 0;
bool     g_sawMoving = false;
ue_wrap::FVector g_levelCentre{};
Approach g_approach;
Step     g_resume = Step::ThrowAim;   // the step a walk to a stand returns to
std::vector<ue_wrap::FVector> g_stands;
size_t   g_standIdx = 0;
bool     g_standsBuilt = false;
std::shared_ptr<DR::BackgroundWalk> g_walk;
std::vector<void*>    g_bricksBefore;
ue_wrap::CachedObjRef g_thrown;
ue_wrap::CachedObjRef g_blast;
ue_wrap::CachedObjRef g_built;

// The fire action's press and release, the pair a left click makes.
DR::InputStub g_firePress{L"InpActEvt_fire_K2Node_InputActionEvent_58"};
DR::InputStub g_fireRelease{L"InpActEvt_fire_K2Node_InputActionEvent_59"};

void To(Step s, uint64_t now) {
    g_step = s;
    g_stepMs = now;
}

const char* Label() { return kLabels[g_pass]; }
const char* LegName(int which) { return kLegs[g_pass][which]; }

// The wall this pass hits: the level's own, then the one the probe placed.
void* Target() { return g_pass == 0 ? LevelWall() : g_built.Get(); }

// The next pass, or the end after the last.
Res NextPass(uint64_t now) {
    if (g_pass == 0) {
        g_pass = 1;
        To(Step::Place, now);
        return Res::Working;
    }
    To(Step::End, now);
    return Res::Done;
}

// The leg's start: the wall brought back to full health when an earlier leg hurt it, so each leg reads from the same
// state, and the routes window opened.
void BeginLeg(const char* leg, void* wall) {
    float h = 0.f, mh = 0.f;
    if (ReadFloat(wall, L"health", h) && ReadFloat(wall, L"maxHealth", mh) && h < mh && WriteFloat(wall, L"health", mh)) {
        Info("leg=%s: the probe restored the wall's health %.1f -> %.1f", leg, static_cast<double>(h),
             static_cast<double>(mh));
        h = mh;
    }
    g_healthBefore = h;
    g_inLeg = true;
    g_leg = leg;
    LegBegin(leg);
}

// The leg's end; true when the wall is still there for the next one.
bool EndLeg(const char* leg, void* wall) {
    g_inLeg = false;
    LegEnd(leg, wall, g_healthBefore);
    return wall && R::IsLive(wall);
}

Res StartWalk(void* me, void* wall, uint64_t now) {
    ue_wrap::FVector mine{}, at{};
    float len = 0.f;
    if (!CentreOf(wall, at) || !E::TryGetActorLocation(me, mine))
        return Invalid("the %s wall or the player did not read", Label());
    if (g_pass == 0) g_levelCentre = at;
    if (!DR::RouteInLegs(me, mine, at, kReachCm, &len))
        return Invalid("no navigation route to within %.0f cm of the %s wall at (%.0f,%.0f,%.0f)",
                       static_cast<double>(kReachCm), Label(), static_cast<double>(at.X), static_cast<double>(at.Y),
                       static_cast<double>(at.Z));
    const int deadline = std::clamp(static_cast<int>(len / 100.f) + 60, kWalkMinS, kWalkMaxS);
    g_walk = DR::StartBackgroundWalk(at, kReachCm, deadline);
    Info("damage: the host walks to within %.0f cm of the %s wall, a %.0f cm route, %d s budget",
         static_cast<double>(kReachCm), Label(), static_cast<double>(len), deadline);
    To(Step::Walk, now);
    return Res::Working;
}

Res AimBound(uint64_t now, const char* what) {
    if (now - g_stepMs < kAimBoundMs) return Res::Working;
    return Invalid("the camera never turned to the %s wall for %s", Label(), what);
}

// Whether the live thrown brick has come to rest, after it was seen moving.
bool AtRest() {
    void* t = g_thrown.Get();
    if (!t) return false;
    const ue_wrap::FVector v = E::GetActorVelocity(t);
    const float speed = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    if (speed > kMovingCmS) g_sawMoving = true;
    g_restReads = speed < kRestCmS ? g_restReads + 1 : 0;
    return g_sawMoving && g_restReads >= kRestTicks;
}

// What the view's line trace strikes: the camera's view as far as `reachCm`, the line the game's attack traces. True when
// the first thing it strikes is `wall`; `said` is the actor's class and distance and whether it is the wall, and `blocker`
// what stands in the way when it is not (the class, or that the line is clear or did not run).
bool ViewNamesWall(void* me, void* wall, float reachCm, std::string& said, std::string& blocker) {
    const ue_wrap::FVector cam = E::GetCameraLocation();
    const ue_wrap::FRotator rot = E::GetCameraRotation();
    const float p = rot.Pitch * kDegToRad, y = rot.Yaw * kDegToRad;
    const ue_wrap::FVector to{cam.X + std::cos(p) * std::cos(y) * reachCm, cam.Y + std::cos(p) * std::sin(y) * reachCm,
                              cam.Z + std::sin(p) * reachCm};
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitStatDyn(me, cam, to, &hit)) {
        said = blocker = "an unreadable view trace";
        return false;
    }
    if (!hit.blocked) {
        said = "the view trace is clear within the reach";
        blocker = "nothing within the reach";
        return false;
    }
    const float dx = hit.point.X - cam.X, dy = hit.point.Y - cam.Y, dz = hit.point.Z - cam.Z;
    const bool named = hit.actor && hit.actor == wall;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "the view trace struck %s at %.0f cm, onWall=%d", ClassOrNull(hit.actor).c_str(),
                  static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)), named ? 1 : 0);
    said = buf;
    blocker = ClassOrNull(hit.actor);
    return named;
}

// The four sides of the wall's box, the host's nearest first: a point kStandOffCm off each face, at the host's height.
std::vector<ue_wrap::FVector> SideStands(void* me, void* wall) {
    ue_wrap::FVector c{}, e{}, mine{};
    if (!E::GetActorBounds(wall, false, c, e) || !E::TryGetActorLocation(me, mine)) return {};
    const float dir[kMaxStands][2] = {{1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}, {0.f, -1.f}};
    std::vector<std::pair<float, ue_wrap::FVector>> sides;
    for (const auto& d : dir) {
        const float reach = (d[0] != 0.f ? e.X : e.Y) + kStandOffCm;
        const ue_wrap::FVector p{c.X + d[0] * reach, c.Y + d[1] * reach, mine.Z};
        sides.emplace_back(std::hypot(p.X - mine.X, p.Y - mine.Y), p);
    }
    std::stable_sort(sides.begin(), sides.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<ue_wrap::FVector> out;
    for (const auto& s : sides) out.push_back(s.second);
    return out;
}

// The leg's stands start over: the sides are measured at its first blocked look.
void ForgetStands() {
    g_stands.clear();
    g_standIdx = 0;
    g_standsBuilt = false;
}

// Starts the director's walk to the next side of the box that a route reaches; false when none is left.
bool StartNextStand(void* me, void* wall, const char* why) {
    if (!g_standsBuilt) {
        g_stands = SideStands(me, wall);
        g_standIdx = 0;
        g_standsBuilt = true;
    }
    while (g_standIdx < g_stands.size()) {
        const ue_wrap::FVector s = g_stands[g_standIdx++];
        ue_wrap::FVector mine{};
        float len = 0.f;
        if (!E::TryGetActorLocation(me, mine) || !DR::RouteInLegs(me, mine, s, kStandReachCm, &len)) continue;
        const int deadline = std::clamp(static_cast<int>(len / 100.f) + 30, 30, 120);
        g_walk = DR::StartBackgroundWalk(s, kStandReachCm, deadline);
        Info("damage: the %s wall is blocked by %s; the host steps to side %zu of %zu at (%.0f,%.0f,%.0f), a %.0f cm route",
             Label(), why, g_standIdx, kMaxStands, static_cast<double>(s.X), static_cast<double>(s.Y),
             static_cast<double>(s.Z), static_cast<double>(len));
        return true;
    }
    return false;
}

// Before a throw or a swing: the view must name the wall. When another actor stands in the way the host walks to the
// next side of the wall's box and the step `resume` looks again from there; `blocker` is what stood in the way.
Sight LookAtWall(void* me, void* wall, float reachCm, Step resume, uint64_t now, std::string& said, std::string& blocker) {
    if (ViewNamesWall(me, wall, reachCm, said, blocker)) return Sight::Named;
    if (!StartNextStand(me, wall, blocker.c_str())) return Sight::Blocked;
    g_resume = resume;
    To(Step::Restand, now);
    return Sight::Walking;
}

// The swing leg's start, after the throw leg ended or was blocked: the axe in the hand and its reach read.
Res AfterThrow(uint64_t now, void* wall) {
    ue_wrap::weapon_catalog::Swing sw;
    if (!ue_wrap::weapon_catalog::Lookup(L"axe", sw) || !(sw.length > 0.f))
        return Invalid("the axe's swing length did not read");
    g_swingReachCm = sw.length;
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"axe", L"prop_axe_C")) return Invalid("the axe could not be taken into the hand");
    g_swings = 0;
    g_nextSwingMs = 0;
    ForgetStands();
    BeginLeg(LegName(1), wall);
    To(Step::Swing, now);
    return Res::Working;
}

}  // namespace

void ResetDamageLegs() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_step = Step::Start;
    g_pass = 0;
    g_stepMs = g_nextSwingMs = 0;
    g_swings = g_restReads = 0;
    g_swingReachCm = 0.f;
    g_sawMoving = g_inLeg = g_thrownSeen = false;
    g_levelCentre = {};
    g_approach.Reset();
    g_resume = Step::ThrowAim;
    ForgetStands();
    g_bricksBefore.clear();
    g_thrown.Reset();
    g_blast.Reset();
    g_built.Reset();
}

Res TickDamageLegs() {
    const uint64_t now = coop::net::NowMs();
    void* me = coop::players::Registry::Get().Local();
    void* wall = Target();
    if (g_step == Step::End) return Res::Done;
    if (!me) return Invalid("the host's player is gone");
    if (!wall && g_step != Step::Start && g_step != Step::Place) {
        // The wall broke: what the leg saw is its result, and nothing is left to hit.
        if (g_inLeg) EndLeg(g_leg, nullptr);
        Info("damage: the %s wall is gone; its legs end here", Label());
        return NextPass(now);
    }
    switch (g_step) {
    case Step::Start:
        if (!wall) return Invalid("no %s customWall_C to hit", Label());
        return StartWalk(me, wall, now);
    case Step::Walk: {
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) return Res::Working;
        if (state == 2) return Invalid("the director's walk to the %s wall did not arrive", Label());
        Info("damage: the host arrived at the %s wall", Label());
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"brick", L"prop_C"))
            return Invalid("a brick could not be taken into the hand");
        ForgetStands();
        To(Step::ThrowAim, now);
        return Res::Working;
    }
    case Step::Restand: {
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) return Res::Working;
        // A stand the walk did not reach is looked at from where the host stands: the look walks on to the next side.
        Info("damage: the host %s its stand beside the %s wall", state == 1 ? "reached" : "did not reach", Label());
        To(g_resume, now);
        return Res::Working;
    }
    case Step::ThrowAim: {
        ue_wrap::FVector at{};
        if (!CentreOf(wall, at)) return Invalid("the %s wall's place did not read", Label());
        FaceTowards(me, at);
        if (now - g_stepMs < kAimHoldMs) return AimBound(now, "the throw");
        if (!ViewOn(at)) return AimBound(now, "the throw");
        std::string said, blocker;
        const Sight sight = LookAtWall(me, wall, kThrowTraceCm, Step::ThrowAim, now, said, blocker);
        if (sight == Sight::Walking) return Res::Working;
        if (sight == Sight::Blocked) {
            Info("damage: the throw at the %s wall is blocked by %s after %zu sides; the leg moves on", Label(),
                 blocker.c_str(), g_standIdx);
            return AfterThrow(now, wall);
        }
        BeginLeg(LegName(0), wall);
        SnapshotBricks(g_bricksBefore);
        if (!DV::Throw(me)) return Invalid("throwHoldingProp did not run");
        g_thrown.Reset();
        g_approach.Reset();
        g_thrownSeen = false;
        g_restReads = 0;
        g_sawMoving = false;
        Info("damage: the host threw a brick at the %s wall", Label());
        To(Step::ThrowSettle, now);
        return Res::Working;
    }
    case Step::ThrowSettle: {
        if (!g_thrownSeen) {
            if (void* b = NewBrick(g_bricksBefore)) {
                g_thrown.Set(b);
                g_thrownSeen = true;
            }
        }
        if (g_thrownSeen && g_thrown.Alive()) g_approach.Sample(g_thrown.Get(), wall);
        // Done when the brick is gone (it broke or was taken) or has come to rest.
        const bool gone = g_thrownSeen && !g_thrown.Alive();
        if (!gone && !(g_thrownSeen && AtRest())) {
            if (now - g_stepMs < kSettleBoundMs) return Res::Working;
            return Invalid(g_thrownSeen ? "the thrown brick never came to rest" : "no thrown brick appeared");
        }
        Info("damage: the thrown brick %s; its distance to the %s wall's box: %s", gone ? "is gone" : "came to rest", Label(),
             g_approach.Say().c_str());
        if (!EndLeg(LegName(0), wall)) return NextPass(now);
        return AfterThrow(now, wall);
    }
    case Step::Swing: {
        ue_wrap::FVector at{};
        if (!CentreOf(wall, at)) return Invalid("the %s wall's place did not read", Label());
        const bool ended = LegRouteFired() || (g_swings >= kMaxSwings && now >= g_nextSwingMs);
        if (ended) {
            if (!EndLeg(LegName(1), wall)) return NextPass(now);
            To(Step::Explode, now);
            return Res::Working;
        }
        if (g_swings >= kMaxSwings || now < g_nextSwingMs) return Res::Working;
        FaceTowards(me, at);
        if (now - g_stepMs < kAimHoldMs || !ViewOn(at)) return AimBound(now, "the swing");
        std::string traced, blocker;
        const Sight sight = LookAtWall(me, wall, g_swingReachCm, Step::Swing, now, traced, blocker);
        if (sight == Sight::Walking) return Res::Working;
        if (sight == Sight::Blocked) {
            Info("damage: the swing at the %s wall is blocked by %s after %zu sides; the leg moves on", Label(),
                 blocker.c_str(), g_standIdx);
            if (!EndLeg(LegName(1), wall)) return NextPass(now);
            To(Step::Explode, now);
            return Res::Working;
        }
        if (!DR::PressInputEvent(me, g_firePress) || !DR::PressInputEvent(me, g_fireRelease))
            return Invalid("the fire input event did not run");
        ++g_swings;
        g_nextSwingMs = now + kSwingGapMs;
        g_stepMs = now;
        Info("damage: swing %d of %d at the %s wall: %s (reach %.0f cm)", g_swings, kMaxSwings, Label(), traced.c_str(),
             static_cast<double>(g_swingReachCm));
        return Res::Working;
    }
    case Step::Explode: {
        ue_wrap::FVector centre{}, mine{};
        if (!CentreOf(wall, centre) || !E::TryGetActorLocation(me, mine)) return Invalid("the wall or the player did not read");
        const float dx = mine.X - centre.X, dy = mine.Y - centre.Y;
        const float flat = std::sqrt(dx * dx + dy * dy);
        if (!(flat > 1.f)) return Invalid("the player stands in the wall's centre: no side to put the blast on");
        const float loc[3] = {centre.X + dx / flat * kBlastStandoffCm, centre.Y + dy / flat * kBlastStandoffCm, centre.Z};
        const float rot[3] = {0.f, 0.f, 0.f}, scale[3] = {1.f, 1.f, 1.f};
        namespace X = ue_wrap::explosion;
        if (!X::ResolveNow(X::Cls::Explosion)) return Invalid("the explosion class did not resolve");
        const X::Params p{kBlastDamage, kBlastRadiusCm, 0.f, 0.f, 0.f, 0.f, 0.f, 0, false, false};
        BeginLeg(LegName(2), wall);
        void* blast = X::SpawnDeferred(X::Cls::Explosion, loc, rot, scale, p, me);
        if (!blast) return Invalid("the explosion did not spawn");
        g_blast.Set(blast);
        Info("damage: an explosion (damage %.0f, radius %.0f cm) spawned %.0f cm from the %s wall's centre, the player ignored",
             static_cast<double>(kBlastDamage), static_cast<double>(kBlastRadiusCm), static_cast<double>(kBlastStandoffCm),
             Label());
        To(Step::ExplodeWait, now);
        return Res::Working;
    }
    case Step::ExplodeWait:
        if (!LegRouteFired() && g_blast.Alive() && R::IsLive(g_blast.Get())) return Res::Working;
        EndLeg(LegName(2), wall);
        coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
        return NextPass(now);
    case Step::Place: {
        ue_wrap::FVector mine{}, placed{};
        if (!E::TryGetActorLocation(me, mine)) return Invalid("the host's place did not read");
        const float dx = mine.X - g_levelCentre.X, dy = mine.Y - g_levelCentre.Y;
        const float flat = std::sqrt(dx * dx + dy * dy);
        if (!(flat > 1.f)) return Invalid("the host stands at the level wall's centre: no side to put its own wall on");
        const ue_wrap::FVector ask{mine.X + dx / flat * kBuiltAwayCm, mine.Y + dy / flat * kBuiltAwayCm, mine.Z};
        void* built = PlaceOnGround(me, kBuiltPath, ask, "the host-built customWall_C", placed);
        if (!built) return Res::Invalid;
        g_built.Set(built);
        float h = -1.f, mh = -1.f;
        ReadFloat(built, L"health", h);
        ReadFloat(built, L"maxHealth", mh);
        Info("damage: the host-built wall stands at (%.1f,%.1f,%.1f), health %.1f of %.1f; its legs begin",
             static_cast<double>(placed.X), static_cast<double>(placed.Y), static_cast<double>(placed.Z),
             static_cast<double>(h), static_cast<double>(mh));
        To(Step::Start, now);
        return Res::Working;
    }
    case Step::End:
        return Res::Done;
    }
    return Res::Working;
}

}  // namespace coop::dev::build_probe::detail
