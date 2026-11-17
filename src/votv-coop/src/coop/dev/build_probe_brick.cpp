// coop/dev/build_probe_brick.cpp -- the building probe's brick leg: an unfinished wall on each peer at one place, and one
// brick thrown by the client into its own copy; see coop/dev/build_probe.h. The host's copy of the brick is the client's
// throw as the host's world makes it, so whether it fires the host wall's overlap is the measurement.

#include "build_probe_detail.h"

#include "coop/dev/drill_hand.h"
#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"

#include "harness/autotest/peer_signal.h"

#include "drive_drill_verbs.h"  // Throw: the hand's throw (co-located private header)

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::build_probe::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
namespace DV = coop::dev::drive_drill_verbs;

constexpr const wchar_t* kWallPath = L"/Game/objects/customWall_unfinished.customWall_unfinished_C";
constexpr const char*    kGoSignal = "build_probe_brick_go";      // host to client: the wall's place, "x y z"
constexpr const char*    kDoneSignal = "build_probe_brick_done";  // client to host: its copy's reading
constexpr float    kAheadCm = 220.f;          // the wall stands this far before the client's puppet
constexpr uint64_t kPollMs = 250;
constexpr uint64_t kAimBoundMs = 10000;
constexpr uint64_t kAimHoldMs = 300;
constexpr uint64_t kSettleBoundMs = 30000;
constexpr uint64_t kClientBoundMs = 180000;   // the host: the client's throw and its reading arrive within this
constexpr uint64_t kMirrorBoundMs = 90000;    // the host: its copy of the brick is seen, and rests or goes, within this
constexpr float    kMirrorRangeCm = 600.f;
constexpr int      kRestTicks = 3;
constexpr float    kRestCmS = 10.f;
constexpr float    kMovingCmS = 50.f;

enum class Step : uint8_t { Spawn, Wait, Aim, Settle, Judge, End };

Step     g_host = Step::Spawn, g_client = Step::Wait;
uint64_t g_stepMs = 0, g_nextPollMs = 0;
ue_wrap::CachedObjRef g_wall;
ue_wrap::FVector      g_at{};
int32_t               g_bricksBefore = -1;
uint32_t              g_overlapsBefore = 0;
std::vector<void*>    g_before;
ue_wrap::CachedObjRef g_brick;
bool                  g_brickSeen = false, g_sawMoving = false;
int                   g_restReads = 0;
std::string           g_clientText;
Approach              g_approach;   // the brick's distance to this peer's wall, read each tick from the throw on

bool Due(uint64_t now) {
    if (now < g_nextPollMs) return false;
    g_nextPollMs = now + kPollMs;
    return true;
}

// "bricks=<n> wall=<alive|gone>" for this peer's copy, with the overlap firings since the leg began and the brick's
// approach to the wall's box.
std::string Reading() {
    int32_t bricks = -1;
    void* w = g_wall.Get();
    const bool alive = w && R::IsLive(w) && ReadInt(w, L"bricks", bricks);
    char buf[200];
    std::snprintf(buf, sizeof(buf), "overlaps=%u bricks=%d->%d wall=%s %s", OverlapFires() - g_overlapsBefore,
                  static_cast<int>(g_bricksBefore), static_cast<int>(alive ? bricks : -1), alive ? "alive" : "gone",
                  g_approach.Say().c_str());
    return buf;
}

// One reading of the live brick against the wall: every tick, so a fast brick's closest approach is not stepped over.
void SampleBrick() {
    if (g_brickSeen && g_brick.Alive()) g_approach.Sample(g_brick.Get(), g_wall.Get());
}

bool AtRest(void* t) {
    const ue_wrap::FVector v = E::GetActorVelocity(t);
    const float speed = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    if (speed > kMovingCmS) g_sawMoving = true;
    g_restReads = speed < kRestCmS ? g_restReads + 1 : 0;
    return g_sawMoving && g_restReads >= kRestTicks;
}

// A live brick of this peer within range of the wall's place that was not there before: the host's copy of the client's
// throw, or the client's own brick.
void* BrickNearWall() {
    std::vector<void*> now;
    SnapshotBricks(now);
    for (void* a : now) {
        bool known = false;
        for (void* b : g_before) known = known || a == b;
        ue_wrap::FVector p{};
        if (known || !E::TryGetActorLocation(a, p)) continue;
        const float dx = p.X - g_at.X, dy = p.Y - g_at.Y;
        if (std::sqrt(dx * dx + dy * dy) < kMirrorRangeCm) return a;
    }
    return nullptr;
}

}  // namespace

void ResetBrick() {
    g_host = Step::Spawn;
    g_client = Step::Wait;
    g_stepMs = g_nextPollMs = 0;
    g_wall.Reset();
    g_brick.Reset();
    g_at = {};
    g_bricksBefore = -1;
    g_overlapsBefore = 0;
    g_before.clear();
    g_brickSeen = g_sawMoving = false;
    g_restReads = 0;
    g_clientText.clear();
    g_approach.Reset();
}

Res TickBrickHost() {
    const uint64_t now = coop::net::NowMs();
    SampleBrick();
    switch (g_host) {
    case Step::Spawn: {
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
        ue_wrap::FVector pos{};
        if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, pos))
            return Res::Working;   // the puppet is not placed yet: the host's join is already over, so it is a frame or two
        const ue_wrap::FVector fwd = E::GetActorForwardVector(puppet);
        void* wall = PlaceOnGround(puppet, kWallPath, {pos.X + fwd.X * kAheadCm, pos.Y + fwd.Y * kAheadCm, pos.Z},
                                   "the host's unfinished wall", g_at);
        if (!wall) return Res::Invalid;   // PlaceOnGround said INVALID
        g_wall.Set(wall);
        ReadInt(wall, L"bricks", g_bricksBefore);
        g_overlapsBefore = OverlapFires();
        SnapshotBricks(g_before);
        char text[96];
        std::snprintf(text, sizeof(text), "%.1f %.1f %.1f", static_cast<double>(g_at.X), static_cast<double>(g_at.Y),
                      static_cast<double>(g_at.Z));
        PS::Mark(kGoSignal, text);
        Info("brick: the host's unfinished wall stands at (%s), bricks=%d; the client is told", text,
             static_cast<int>(g_bricksBefore));
        g_stepMs = now;
        g_host = Step::Wait;
        return Res::Working;
    }
    case Step::Wait: {
        if (!Due(now)) return Res::Working;
        if (!g_brickSeen) {
            if (void* b = BrickNearWall()) {
                g_brick.Set(b);
                g_brickSeen = true;
                Info("brick: the host's copy of a brick appeared near its wall");
            }
        }
        if (!PS::Peek(kDoneSignal, g_clientText)) {
            if (now - g_stepMs >= kClientBoundMs) return Invalid("the client's brick reading never came");
            return Res::Working;
        }
        Info("brick: the client reports %s", g_clientText.c_str());
        g_stepMs = now;
        g_host = Step::Judge;
        return Res::Working;
    }
    case Step::Judge: {
        if (!Due(now)) return Res::Working;
        if (!g_brickSeen) {
            if (void* b = BrickNearWall()) {
                g_brick.Set(b);
                g_brickSeen = true;
            }
        }
        const bool gone = g_brickSeen && !g_brick.Alive();
        const bool rested = g_brickSeen && !gone && AtRest(g_brick.Get());
        if (!gone && !rested) {
            if (now - g_stepMs < kMirrorBoundMs) return Res::Working;
            return Invalid(g_brickSeen ? "the host's copy of the brick never came to rest" : "the host never had a copy of the brick");
        }
        Info("brick: the host's copy of the brick %s; host wall: %s", gone ? "is gone" : "came to rest",
             Reading().c_str());
        g_host = Step::End;
        return Res::Done;
    }
    default:
        return Res::Done;
    }
}

Res TickBrickClient() {
    const uint64_t now = coop::net::NowMs();
    void* me = coop::players::Registry::Get().Local();
    switch (g_client) {
    case Step::Wait: {
        if (!Due(now)) return Res::Working;
        std::string text;
        if (!PS::Peek(kGoSignal, text)) return Res::Working;
        const char* p = text.c_str();
        char* end = nullptr;
        float v[3] = {};
        for (float& f : v) {
            f = std::strtof(p, &end);
            if (end == p) return Invalid("the host's wall place did not parse: %s", text.c_str());
            p = end;
        }
        const float x = v[0], y = v[1], z = v[2];
        if (!me) return Invalid("the client's player is gone");
        void* wall = PlaceOnGround(me, kWallPath, {x, y, z}, "the client's unfinished wall", g_at);
        if (!wall) return Res::Invalid;   // PlaceOnGround said INVALID
        g_wall.Set(wall);
        ReadInt(wall, L"bricks", g_bricksBefore);
        g_overlapsBefore = OverlapFires();
        SnapshotBricks(g_before);
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"brick", L"prop_C")) return Invalid("a brick could not be taken into the hand");
        Info("brick: the client's unfinished wall stands at (%s), bricks=%d", text.c_str(), static_cast<int>(g_bricksBefore));
        g_stepMs = now;
        g_client = Step::Aim;
        return Res::Working;
    }
    case Step::Aim: {
        void* wall = g_wall.Get();
        ue_wrap::FVector at{};
        if (!me || !wall || !CentreOf(wall, at)) return Invalid("the client's player or wall is gone before the throw");
        FaceTowards(me, at);
        if (now - g_stepMs < kAimHoldMs || !ViewOn(at)) {
            if (now - g_stepMs >= kAimBoundMs) return Invalid("the camera never turned to the client's wall");
            return Res::Working;
        }
        if (!DV::Throw(me)) return Invalid("throwHoldingProp did not run");
        Info("brick: the client threw a brick at its wall");
        g_stepMs = now;
        g_client = Step::Settle;
        return Res::Working;
    }
    case Step::Settle: {
        if (!g_brickSeen) {
            if (void* b = NewBrick(g_before)) {
                g_brick.Set(b);
                g_brickSeen = true;
            }
        }
        SampleBrick();
        const bool gone = g_brickSeen && !g_brick.Alive();
        const bool rested = g_brickSeen && !gone && AtRest(g_brick.Get());
        if (!gone && !rested) {
            if (now - g_stepMs < kSettleBoundMs) return Res::Working;
            return Invalid(g_brickSeen ? "the client's brick never came to rest" : "no thrown brick appeared on the client");
        }
        char text[200];
        std::snprintf(text, sizeof(text), "brick=%s %s", gone ? "gone" : "rested", Reading().c_str());
        Info("brick: the client's copy: %s", text);
        PS::Mark(kDoneSignal, text);
        g_client = Step::End;
        return Res::Done;
    }
    default:
        return Res::Done;
    }
}

}  // namespace coop::dev::build_probe::detail
