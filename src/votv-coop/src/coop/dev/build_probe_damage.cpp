// coop/dev/build_probe_damage.cpp -- the host's three legs at the level's wall: a thrown brick, a swung axe, an explosion
// beside it; see coop/dev/build_probe.h. The director walks the host to the wall, never a teleport. A leg that destroys
// the wall ends the legs there: what fired is the result, and nothing is left to hit.

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
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/explosion.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace coop::dev::build_probe::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DR = coop::director;
namespace DV = coop::dev::drive_drill_verbs;

enum class Step : uint8_t { Start, Walk, ThrowAim, ThrowSettle, Swing, Explode, ExplodeWait, End };

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

Step     g_step = Step::Start;
uint64_t g_stepMs = 0;
uint64_t g_nextSwingMs = 0;
int      g_swings = 0;
float    g_healthBefore = 0.f;
bool     g_inLeg = false;
const char* g_leg = "";
bool     g_thrownSeen = false;
int      g_restReads = 0;
bool     g_sawMoving = false;
std::shared_ptr<DR::BackgroundWalk> g_walk;
std::vector<void*>    g_bricksBefore;
ue_wrap::CachedObjRef g_thrown;
ue_wrap::CachedObjRef g_blast;

// The fire action's press and release, the pair a left click makes.
DR::InputStub g_firePress{L"InpActEvt_fire_K2Node_InputActionEvent_58"};
DR::InputStub g_fireRelease{L"InpActEvt_fire_K2Node_InputActionEvent_59"};

void To(Step s, uint64_t now) {
    g_step = s;
    g_stepMs = now;
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
    if (!CentreOf(wall, at) || !E::TryGetActorLocation(me, mine)) return Invalid("the level wall or the player did not read");
    if (!DR::RouteInLegs(me, mine, at, kReachCm, &len))
        return Invalid("no navigation route to within %.0f cm of the level wall at (%.0f,%.0f,%.0f)",
                       static_cast<double>(kReachCm), static_cast<double>(at.X), static_cast<double>(at.Y),
                       static_cast<double>(at.Z));
    const int deadline = std::clamp(static_cast<int>(len / 100.f) + 60, kWalkMinS, kWalkMaxS);
    g_walk = DR::StartBackgroundWalk(at, kReachCm, deadline);
    Info("damage: the host walks to within %.0f cm of the level wall, a %.0f cm route, %d s budget",
         static_cast<double>(kReachCm), static_cast<double>(len), deadline);
    To(Step::Walk, now);
    return Res::Working;
}

Res AimBound(uint64_t now, const char* what) {
    if (now - g_stepMs < kAimBoundMs) return Res::Working;
    return Invalid("the camera never turned to the wall for %s", what);
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

}  // namespace

void ResetDamageLegs() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_step = Step::Start;
    g_stepMs = g_nextSwingMs = 0;
    g_swings = g_restReads = 0;
    g_sawMoving = g_inLeg = g_thrownSeen = false;
    g_bricksBefore.clear();
    g_thrown.Reset();
    g_blast.Reset();
}

Res TickDamageLegs() {
    const uint64_t now = coop::net::NowMs();
    void* me = coop::players::Registry::Get().Local();
    void* wall = LevelWall();
    if (g_step == Step::End) return Res::Done;
    if (!me) return Invalid("the host's player is gone");
    if (!wall && g_step != Step::Start) {
        // The wall broke: what the leg saw is its result, and nothing is left to hit.
        if (g_inLeg) EndLeg(g_leg, nullptr);
        Info("damage: the level wall is gone; the legs end here");
        To(Step::End, now);
        return Res::Done;
    }
    switch (g_step) {
    case Step::Start:
        if (!wall) return Invalid("no customWall_C in the persistent level to hit");
        return StartWalk(me, wall, now);
    case Step::Walk: {
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) return Res::Working;
        if (state == 2) return Invalid("the director's walk to the level wall did not arrive");
        Info("damage: the host arrived at the wall");
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"brick", L"prop_C"))
            return Invalid("a brick could not be taken into the hand");
        To(Step::ThrowAim, now);
        return Res::Working;
    }
    case Step::ThrowAim: {
        ue_wrap::FVector at{};
        if (!CentreOf(wall, at)) return Invalid("the level wall's place did not read");
        FaceTowards(me, at);
        if (now - g_stepMs < kAimHoldMs) return AimBound(now, "the throw");
        if (!ViewOn(at)) return AimBound(now, "the throw");
        BeginLeg("throw", wall);
        SnapshotBricks(g_bricksBefore);
        if (!DV::Throw(me)) return Invalid("throwHoldingProp did not run");
        g_thrown.Reset();
        g_thrownSeen = false;
        g_restReads = 0;
        g_sawMoving = false;
        Info("damage: the host threw a brick at the wall");
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
        // Done when the brick is gone (it broke or was taken) or has come to rest.
        const bool gone = g_thrownSeen && !g_thrown.Alive();
        if (!gone && !(g_thrownSeen && AtRest())) {
            if (now - g_stepMs < kSettleBoundMs) return Res::Working;
            return Invalid(g_thrownSeen ? "the thrown brick never came to rest" : "no thrown brick appeared");
        }
        if (!EndLeg("throw", wall)) {
            To(Step::End, now);
            return Res::Done;
        }
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"axe", L"prop_axe_C"))
            return Invalid("the axe could not be taken into the hand");
        g_swings = 0;
        g_nextSwingMs = 0;
        BeginLeg("swing", wall);
        To(Step::Swing, now);
        return Res::Working;
    }
    case Step::Swing: {
        ue_wrap::FVector at{};
        if (!CentreOf(wall, at)) return Invalid("the level wall's place did not read");
        const bool ended = LegRouteFired() || (g_swings >= kMaxSwings && now >= g_nextSwingMs);
        if (ended) {
            if (!EndLeg("swing", wall)) {
                To(Step::End, now);
                return Res::Done;
            }
            To(Step::Explode, now);
            return Res::Working;
        }
        if (g_swings >= kMaxSwings || now < g_nextSwingMs) return Res::Working;
        FaceTowards(me, at);
        if (now - g_stepMs < kAimHoldMs || !ViewOn(at)) return AimBound(now, "the swing");
        if (!DR::PressInputEvent(me, g_firePress) || !DR::PressInputEvent(me, g_fireRelease))
            return Invalid("the fire input event did not run");
        ++g_swings;
        g_nextSwingMs = now + kSwingGapMs;
        g_stepMs = now;
        Info("damage: swing %d of %d", g_swings, kMaxSwings);
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
        BeginLeg("explosion", wall);
        void* blast = X::SpawnDeferred(X::Cls::Explosion, loc, rot, scale, p, me);
        if (!blast) return Invalid("the explosion did not spawn");
        g_blast.Set(blast);
        Info("damage: an explosion (damage %.0f, radius %.0f cm) spawned %.0f cm from the wall's centre, the player ignored",
             static_cast<double>(kBlastDamage), static_cast<double>(kBlastRadiusCm), static_cast<double>(kBlastStandoffCm));
        To(Step::ExplodeWait, now);
        return Res::Working;
    }
    case Step::ExplodeWait:
        if (!LegRouteFired() && g_blast.Alive() && R::IsLive(g_blast.Get())) return Res::Working;
        EndLeg("explosion", wall);
        coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
        To(Step::End, now);
        return Res::Done;
    case Step::End:
        return Res::Done;
    }
    return Res::Working;
}

}  // namespace coop::dev::build_probe::detail
