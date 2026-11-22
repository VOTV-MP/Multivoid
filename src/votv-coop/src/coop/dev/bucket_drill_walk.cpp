// coop/dev/bucket_drill_walk.cpp -- the client's approach to a staged prop before it acts on it; see bucket_drill_detail.h.
//
// A client's verb on a bucket or a bag reaches it through the player's own look-at trace (200 cm from the camera), so the client stands
// where its camera is near the prop, as a player does: the director walks it (a background walk, the walk-to set, or the carry-to set
// while the hand holds an item) to a standpoint of a ring about the prop that a route reaches (ReachableStandpoints, as the clean drill
// does), the next standpoint when a walk fails or ends out of reach, a second, wider ring when the first is spent, and the arm says
// INVALID only when both rings are.

#include "bucket_drill_detail.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/player/hand_item.h"
#include "coop/props/container_reach.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::bucket_drill::detail {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace HB = coop::hand_item;

namespace {

constexpr float kStandCm = 170.f;        // a client whose camera is nearer than this to the prop's origin acts where it stands
constexpr float kLookAtCm = coop::props::container_reach::kReachUU;   // a standpoint that leaves the camera farther than this is not used
constexpr float kMinRingCm = 80.f;       // a ring nearer than this puts the walker's body into a bucket's
constexpr float kSecondRingCm = 110.f;   // the ring tried when the first serves no standpoint: its camera stands sqrt(110^2 + 156^2) = 191 cm from the origin
constexpr float kArriveCm = 20.f;        // a walker is at a standpoint within this (level); the ring is sized for the worst such stop
constexpr float kClearStandCm = 50.f;    // a standpoint nearer than this (level) to another staged prop puts the walker in it
constexpr int   kRingPoints = 8;
constexpr int   kMaxStands = 4;          // the standpoints tried about one prop
constexpr int   kWalkDeadlineS = 10;

struct State {
    void* target = nullptr;
    int rings = 0;            // the rings that have been asked for: 0 none, 1 the first, 2 the second as well
    size_t offeredFirst = 0;  // the standpoints the first ring offered, for the line that says INVALID
    bool arrived = false;     // the camera stands at a standpoint within kLookAtCm of the target
    bool mustWalk = false;    // the standpoint the client stands at did not serve: the next is walked to
    bool faced = false;       // the camera has been turned to the target
    std::vector<ue_wrap::FVector> ring;   // the standpoints, shortest route first
    size_t stand = 0;                     // the one being walked to or stood at
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    int walksFailed = 0, aimsFailed = 0;
    std::string lastAim;
};
State g_s;

float DistFrom(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The player's camera, the point its look-at trace is cast from; false while it reads as unplaced.
bool EyeOf(void* player, ue_wrap::FVector& eye) {
    return E::ReadMainPlayerCameraLocation(player, eye) && std::isfinite(eye.X) && std::isfinite(eye.Y) && std::isfinite(eye.Z) &&
           !(eye.X == 0.f && eye.Y == 0.f && eye.Z == 0.f);
}

void CancelWalk() {
    if (g_s.walk && g_s.walk->state.load() == 0) coop::director::CancelBackgroundWalk(*g_s.walk);
}

// The standpoints about the target that a route reaches and that stand clear of every other staged prop, shortest route first.
// `reached` takes how many of the ring's points a route reached.
std::vector<ue_wrap::FVector> StandpointsAbout(void* me, void* target, const ue_wrap::FVector& at, float ringCm, size_t& reached) {
    std::vector<ue_wrap::FVector> ring = coop::director::ReachableStandpoints(me, at, ringCm, kRingPoints, kArriveCm);
    reached = ring.size();
    std::vector<ue_wrap::FVector> others;
    void* const hand = HB::LocalHandActor();   // the held item travels with the player: it is no obstacle
    for (int slot = 0; slot < kMaxPlains + kMaxItems; ++slot) {
        void* const p = LiveProp(ClientEid(slot));
        ue_wrap::FVector pl{};
        if (p && p != target && p != hand && E::TryGetActorLocation(p, pl)) others.push_back(pl);
    }
    // The reachable standpoints, the clearest first: one nearer than kClearStandCm to another staged prop would put the walker's
    // body into it and is not offered; the rest are tried in order of their room, so a crowded stage still offers its best.
    std::vector<std::pair<float, ue_wrap::FVector>> ranked;
    for (const ue_wrap::FVector& s : ring) {
        float room = 1e9f;
        for (const ue_wrap::FVector& o : others) room = (std::min)(room, std::hypot(s.X - o.X, s.Y - o.Y));
        if (room >= kClearStandCm) ranked.emplace_back(room, s);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::vector<ue_wrap::FVector> out;
    for (const auto& r : ranked)
        if (out.size() < static_cast<size_t>(kMaxStands)) out.push_back(r.second);
    return out;
}

// The ring of `ringCm` about the target is asked for: its standpoints replace the list, and the next one to walk to is the first.
void BuildRing(void* me, void* target, const ue_wrap::FVector& at, float dz, float ringCm) {
    size_t reached = 0;
    g_s.ring = StandpointsAbout(me, target, at, ringCm, reached);
    ++g_s.rings;
    g_s.stand = 0;
    Fact("a route reaches %zu of the %d standpoints on a %.0f cm ring about %ls (element %d), %zu of them clear of the other staged props; "
         "the camera is %.0f cm above the prop's origin",
         reached, kRingPoints, static_cast<double>(ringCm), R::ClassNameOf(target).c_str(), EidOf(target), g_s.ring.size(), static_cast<double>(dz));
}

void InvalidNone(void* target, const char* why) {
    char text[640];
    std::snprintf(text, sizeof(text), "no standpoint about %ls (element %d) serves the client: %s", R::ClassNameOf(target).c_str(), EidOf(target), why);
    Invalid(text);
}

}  // namespace

// The state is per target: a new target starts over. Order each tick: a walk in flight is polled; a camera near enough is turned to the
// target (one tick), then the caller acts; otherwise the ring is built once and the next standpoint is walked to.
int Approach(void* me, void* target) {
    if (!me || !target) {
        Invalid("this client's player or the staged prop it acts on is gone");
        return -1;
    }
    if (g_s.target != target) {
        CancelWalk();
        g_s = State{};
        g_s.target = target;
    }
    ue_wrap::FVector eye{}, at{};
    if (!EyeOf(me, eye) || !E::TryGetActorLocation(target, at)) {
        Late("this client's camera or the staged prop it acts on could not be read", kBoundTicks / 2);
        return 0;
    }
    const float dist = DistFrom(eye, at);
    if (g_s.walk) {
        const int st = g_s.walk->state.load();
        if (st == 0) return 0;
        g_s.walk.reset();
        if (st == 2) {   // this standpoint was not reached: the next
            Fact("the walk to standpoint %zu about %ls (element %d) did not reach it", g_s.stand, R::ClassNameOf(target).c_str(), EidOf(target));
            ++g_s.walksFailed;
            ++g_s.stand;
            return 0;
        }
        if (dist > kLookAtCm) {   // reached, but the camera stands out of the look-at's reach
            Fact("the camera stands %.0f cm from %ls (element %d) at standpoint %zu, past the look-at reach", static_cast<double>(dist),
                 R::ClassNameOf(target).c_str(), EidOf(target), g_s.stand);
            ++g_s.walksFailed;
            ++g_s.stand;
            return 0;
        }
        g_s.arrived = true;
        g_s.mustWalk = false;
        Fact("the client stands at standpoint %zu, its camera %.0f cm from %ls (element %d)", g_s.stand, static_cast<double>(dist),
             R::ClassNameOf(target).c_str(), EidOf(target));
    }
    if (!g_s.mustWalk && dist <= (g_s.arrived ? kLookAtCm : kStandCm)) {
        if (!g_s.faced) {   // turned to the target one tick before the caller acts
            E::SetControlRotation(E::GetController(me), coop::director::LookAt(eye, at));
            g_s.faced = true;
            return 0;
        }
        return 1;
    }
    g_s.arrived = false;
    g_s.faced = false;
    const float dz = eye.Z - at.Z;
    if (g_s.rings == 0) {
        // The ring's radius puts the camera kStandCm from the origin when the walker stops kArriveCm outside it, but not nearer than the
        // walker's body and the prop's allow: the camera is the player's own height above the prop's origin.
        const float flat = dz < kStandCm ? std::sqrt(kStandCm * kStandCm - dz * dz) - kArriveCm : 0.f;
        BuildRing(me, target, at, dz, std::fmax(kMinRingCm, flat));
    }
    if (g_s.stand >= g_s.ring.size() && g_s.rings == 1) {   // the first ring is spent: the wider one is asked for, in the same order
        g_s.offeredFirst = g_s.ring.size();
        BuildRing(me, target, at, dz, kSecondRingCm);
    }
    if (g_s.stand >= g_s.ring.size()) {
        char why[480];
        std::snprintf(why, sizeof(why),
                      "%zu standpoints were offered on the first ring and %zu on the %.0f cm ring, %d walks failed or ended out of reach, %d aims "
                      "failed (the last: %s)",
                      g_s.offeredFirst, g_s.ring.size(), static_cast<double>(kSecondRingCm), g_s.walksFailed, g_s.aimsFailed,
                      g_s.lastAim.empty() ? "none" : g_s.lastAim.c_str());
        InvalidNone(target, why);
        return -1;
    }
    g_s.walk = coop::director::StartBackgroundWalk(g_s.ring[g_s.stand], kArriveCm, kWalkDeadlineS, HB::LocalHandActor() != nullptr, target);
    return 0;
}

void ApproachRetry(const std::string& why) {
    ++g_s.aimsFailed;
    g_s.lastAim = why;
    if (g_s.arrived) ++g_s.stand;   // the standpoint stood at did not serve: the next one
    g_s.arrived = false;
    g_s.mustWalk = true;
    g_s.faced = false;
}

void ApproachReset() {
    CancelWalk();
    g_s = State{};
}

}  // namespace coop::dev::bucket_drill::detail
