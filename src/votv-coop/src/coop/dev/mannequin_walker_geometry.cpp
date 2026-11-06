// coop/dev/mannequin_walker_geometry.cpp -- see coop/dev/mannequin_walker_geometry.h.

#include "coop/dev/mannequin_walker_geometry.h"

#include "coop/dev/director/director.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/shelter_spot.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace coop::dev::mannequin_walker_drill {
namespace {

namespace DIR = coop::director;
namespace E   = ue_wrap::engine;
namespace PS  = harness::autotest::peer_signal;
namespace R   = ue_wrap::reflection;
namespace V   = ue_wrap::vitals;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr float kAimUpCm = 90.f;   // the mesh's middle, over the actor's origin

// The game's ticker_foodSleeper_C drops a player under 20 food into a ragdoll at random; a ragdoll turns no view and walks nowhere.
void KeepFed() {
    V::Write(V::Field::Food, 100.f);
    V::Write(V::Field::Sleep, 100.f);
}

}  // namespace

bool Invalid(const char* why) {
    UE_LOGW("mannequin-drill: INVALID -- %s", why);
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "%s", why);
    return false;
}

float Dist3(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Aim(void* walker, bool toward) {
    KeepFed();
    void* pawn = coop::players::Registry::Get().Local();
    FVector at{};
    if (!pawn || !walker || !R::IsLive(walker) || !E::TryGetActorLocation(walker, at)) return;
    FRotator rot = DIR::LookAt(E::GetCameraLocation(), FVector{at.X, at.Y, at.Z + kAimUpCm});
    if (!toward) {
        rot.Yaw += 180.f;
        rot.Pitch = 0.f;
    }
    while (rot.Yaw > 180.f) rot.Yaw -= 360.f;
    while (rot.Yaw < -180.f) rot.Yaw += 360.f;
    E::SetControlRotation(E::GetController(pawn), rot);
}

float PlayerTo(void* actor) {
    void* me = coop::players::Registry::Get().Local();
    FVector a{}, b{};
    return me && actor && E::TryGetActorLocation(me, a) && E::TryGetActorLocation(actor, b) ? Dist3(a, b) : -1.f;
}

float ClientTo(void* actor) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(kClientSlot);
    void* puppet = rp && rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
    FVector a{}, b{};
    return puppet && actor && E::TryGetActorLocation(puppet, a) && E::TryGetActorLocation(actor, b) ? Dist3(a, b) : -1.f;
}

bool BedAnchor(FVector& out) {
    bool bed = false;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        if (!me) return;
        const coop::shelter_spot::Target t = coop::shelter_spot::Choose(me);
        if (t.kind != coop::shelter_spot::Kind::Bed) return;
        out = FVector{t.x, t.y, t.z};
        bed = true;
    });
    return bed;
}

bool EnterRange(void* walker, const char* who) {
    FVector me{}, at{};
    float   cm = -1.f;
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        cm = pawn && walker && E::TryGetActorLocation(pawn, me) && E::TryGetActorLocation(walker, at) ? Dist3(me, at) : -1.f;
    });
    char why[200];
    if (cm < 0.f) {
        std::snprintf(why, sizeof(why), "%s: the walker's place or its own did not read before stepping into range", who);
        return Invalid(why);
    }
    if (cm < kInRangeCm) return true;
    // The walk goes toward the walker itself and ends as it comes within kInRangeCm: the NavMesh's own way there. A point on the
    // straight line from the walker is not one (from the bed it lies in the house's walls: no route, measured pr84 s5).
    char what[96];
    std::snprintf(what, sizeof(what), "%s into the walker's range", who);
    if (WalkTo(at, kInRangeCm, what)) return true;
    std::snprintf(why, sizeof(why), "%s did not arrive stepping into range from %.0f cm of the walker", who, cm);
    return Invalid(why);
}

bool WalkTo(const FVector& to, float reachCm, const char* what) {
    std::shared_ptr<DIR::BackgroundWalk> walk;
    OnGameThread([&] { walk = DIR::StartBackgroundWalk(to, reachCm, kWalkDeadlineS); });
    const bool done = PS::WaitFor(what, kWalkWaitS, [&] { return walk->state.load() != 0; }, false);
    if (!done) DIR::CancelBackgroundWalk(*walk);
    UE_LOGI("mannequin-drill: walk '%s' %s", what, done && walk->state.load() == 1 ? "arrived" : "did not arrive");
    return done && walk->state.load() == 1;
}

}  // namespace coop::dev::mannequin_walker_drill
