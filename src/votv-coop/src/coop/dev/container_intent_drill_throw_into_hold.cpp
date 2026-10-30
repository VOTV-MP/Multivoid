// coop/dev/container_intent_drill_throw_into_hold.cpp -- the throwinto control's hold loop: the camera is turned at the point P over
// the target's box, and the player's grabLen (the scroll wheel's own variable) is set to the camera-to-P distance, so the hold point
// (the camera plus its forward times grabLen) IS P. The held drive's own place is read once it has stopped moving; within kHoldOkCm
// of P horizontally and above the box's top face it is ready to be let go (the client lets go). A P outside the game's grabLen range
// is walked to again, the standpoint moved by the excess, at most kMaxCorrections times.

#include "container_intent_drill_throw_into.h"

#include "coop/dev/container_route_census.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace coop::dev::container_intent_drill::detail::into {
namespace {

namespace E  = ue_wrap::engine;
namespace DR = coop::director;

constexpr float    kOverRimCm        = 10.f;    // the point the held drive is brought to: this far above the box's top face
constexpr float    kHoldOkCm         = 10.f;    // let go within this of it, horizontally, and above the top face
constexpr float    kHoldDistCm       = 100.f;   // the camera-to-point distance the standpoint aims at: the middle of the game's 50..150 grabLen
constexpr float    kGrabLenMin       = 50.f;    // the game's clamp of the scroll wheel's own variable
constexpr float    kGrabLenMax       = 150.f;
constexpr int      kMaxCorrections   = 3;       // walks after the first, when the point is out of the grabLen range
constexpr int      kCorrectDeadlineS = 40;
constexpr float    kCorrectReachCm   = 60.f;
constexpr int      kStableTicks      = 8;       // the held drive moved under kStableCm for this many ticks: it is read
constexpr float    kStableCm         = 2.f;
constexpr uint64_t kHoldBoundMs      = 90000;
constexpr uint64_t kInsideWaitMs     = 8000;    // inside: the drive stayed held at the point this long and was not filed
constexpr float    kInsideOkCm       = 20.f;    // inside: the held drive is at the point within this (3-D)
constexpr uint64_t kStillMs          = 1000;    // inside: the held drive stood still outside the box this long: it is lifted and lowered again
constexpr float    kLiftCm           = 20.f;    // the lift: the hold point goes this far above the box's centre
constexpr int      kMaxLifts         = 3;       // lifts in all: the parked host copy gets a fresh BeginOverlap on each pass through the box
constexpr uint64_t kLiftBoundMs      = 1500;    // a lift ends when the drive is at the raised point or after this
constexpr uint64_t kInsideLogMs      = 500;     // inside: the held drive's place is logged this often (diagnostic)

FVector  g_holdT{}, g_holdPrev{};   // the point over the box the held drive is brought to; the held drive's last place
float    g_holdTop = 0.f;           // the box's top face
int      g_holdStable = 0, g_corrections = 0, g_r = 0, g_t = 0;
bool     g_inside = false;          // the point is the box's centre and the drive is never let go (n=3)
uint64_t g_reachedMs = 0;           // inside: the clock at which the held drive first stood at the point
TriggerBox g_hbox;                  // inside: the box in the world, and the height of its centre (the hold point after a lift)
float    g_centreZ = 0.f;
int      g_lifts = 0;               // inside: lifts done
bool     g_lifting = false;         // inside: the hold point is raised
uint64_t g_liftMs = 0, g_stillMs = 0, g_logMs = 0;   // inside: the lift's start; when the drive last moved; the last place line
uint64_t g_heldAll0 = 0;            // the census's overlap count when the drive was first held: what it took while held is read at the release
uint64_t g_startMs = 0;
std::shared_ptr<DR::BackgroundWalk> g_walk;

// The player's grabLen, the scroll wheel's own variable (the game clamps it to 50..150): the hold point moves with it.
void WriteGrabLen(void* player, float v) {
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_grabLen();
    if (off >= 0) *reinterpret_cast<float*>(static_cast<uint8_t*>(player) + off) = std::clamp(v, kGrabLenMin, kGrabLenMax);
}

}  // namespace

// The point the control brings the held drive to: over the trigger box's REAL world centre, kOverRimCm above the box's top face
// (its centre height plus the half-extent's reach along world Z, the box's rotation taken in).
bool HoldReadPoint(void* container, bool inside, void* item) {
    TriggerBox box;
    if (!ReadTriggerBox(container, box)) return false;
    float above = kOverRimCm;
    FVector origin{}, ext{};
    if (item && E::GetActorBounds(item, false, origin, ext)) above += std::sqrt(ext.X * ext.X + ext.Y * ext.Y + ext.Z * ext.Z);
    g_holdTop = box.loc.Z + box.zReach;
    g_holdT = {box.loc.X, box.loc.Y, inside ? box.loc.Z : g_holdTop + above};
    g_hbox = box;
    g_centreZ = box.loc.Z;
    return true;
}

// The horizontal distance from the hold point the standpoint aims at (the camera kHoldDistCm from it), `fallbackCm` when the
// camera is higher above the point than that.
float HoldStandCm(float fallbackCm) {
    const float dz = E::GetCameraLocation().Z - g_holdT.Z;
    return kHoldDistCm > std::fabs(dz) ? std::sqrt(kHoldDistCm * kHoldDistCm - dz * dz) : fallbackCm;
}

bool HoldBegin(void* container, int r, int t, bool inside, void* item) {
    g_holdStable = g_corrections = 0;
    g_inside = inside;
    g_reachedMs = 0;
    g_lifts = 0;
    g_lifting = false;
    g_liftMs = g_stillMs = g_logMs = 0;
    g_r = r;
    g_t = t;
    g_heldAll0 = cen::R4ItemOverlaps();
    g_holdPrev = FVector{};
    g_startMs = ::GetTickCount64();
    return HoldReadPoint(container, inside, item);
}

HoldState HoldTick(void* player, void* item, char* why, size_t whyLen) {
    if (g_walk) {   // a correcting walk
        if (g_walk->state.load() == 0) return HoldState::Working;
        g_walk.reset();
        g_holdStable = 0;
    }
    const FVector cam = E::GetCameraLocation();
    E::SetControlRotation(E::GetController(player), DR::LookAt(cam, g_holdT));
    const float dist = Dist3(cam, g_holdT);
    if (dist < kGrabLenMin || dist > kGrabLenMax) {
        FVector p{};
        float ux = 0.f, uy = 0.f, len = 0.f;
        if (g_corrections >= kMaxCorrections) {
            std::snprintf(why, whyLen, "the hold point stayed %.0f cm from the camera, outside the grabLen range %.0f..%.0f, after %d correcting "
                          "walk(s)", static_cast<double>(dist), static_cast<double>(kGrabLenMin), static_cast<double>(kGrabLenMax), g_corrections);
            return HoldState::Failed;
        }
        if (!E::TryGetActorLocation(player, p) || !Unit(p, g_holdT, ux, uy, len)) {
            std::snprintf(why, whyLen, "the player's place does not read or is on the hold point");
            return HoldState::Failed;
        }
        const float dz = cam.Z - g_holdT.Z;
        const float wantHoriz = kHoldDistCm > std::fabs(dz) ? std::sqrt(kHoldDistCm * kHoldDistCm - dz * dz) : 0.f;
        const float move = len - wantHoriz;   // too far: toward the box; too near: away from it
        g_walk = DR::StartBackgroundWalk(FVector{p.X + ux * move, p.Y + uy * move, p.Z}, kCorrectReachCm, kCorrectDeadlineS, /*carry=*/true);
        ++g_corrections;
        g_holdStable = 0;
        UE_LOGI("[CI-DRILL] client %s n=%d try %d: the hold point is %.0f cm from the camera, outside the grabLen range; walk %d moves the "
                "standpoint %.0f cm", ArmName(), g_r, g_t, static_cast<double>(dist), g_corrections, static_cast<double>(move));
        return HoldState::Working;
    }
    E::MainPlayerGrabState gs{};
    if (E::ReadMainPlayerGrabState(player, gs) && std::fabs(gs.grabLen - dist) > 0.5f) WriteGrabLen(player, dist);
    FVector h{};
    if (!item) {   // the prop is gone: the host filed it while it was held (SP parity for a held prop in an armed box) and its destroy came here
        UE_LOGI("[CI-DRILL] client %s n=%d try %d: the held drive is gone, filed by the host while held (%s), the grab reads %s", ArmName(), g_r, g_t,
                g_inside ? "the held-insert case" : "before the release", DR::Grabbing(player) ? "still set" : "null");
        return HoldState::Ready;
    }
    if (DR::Grabbing(player) != item || !E::TryGetActorLocation(item, h)) {
        std::snprintf(why, whyLen, "the grab was lost before the drop");
        return HoldState::Failed;
    }
    if (g_inside) {   // lowered into the box and held: Ready is the prop's death above, or the wait passing with the drive at the point
        const uint64_t now = ::GetTickCount64();
        const bool inBox = PointInBox(g_hbox, h);
        if (now - g_logMs >= kInsideLogMs) {   // diagnostic: where the held drive is, against the hold point and the box
            g_logMs = now;
            UE_LOGI("[CI-DRILL] client %s n=%d try %d: held drive at (%.1f,%.1f,%.1f), %.1f cm from the hold point (%.1f,%.1f,%.1f), %s the box, "
                    "%d lift(s)%s", ArmName(), g_r, g_t, static_cast<double>(h.X), static_cast<double>(h.Y), static_cast<double>(h.Z),
                    static_cast<double>(Dist3(h, g_holdT)), static_cast<double>(g_holdT.X), static_cast<double>(g_holdT.Y),
                    static_cast<double>(g_holdT.Z), inBox ? "in" : "outside", g_lifts, g_lifting ? ", lifted" : "");
        }
        if (!g_stillMs || Dist3(h, g_holdPrev) >= kStableCm) {
            g_stillMs = now;
            g_holdPrev = h;
        }
        if (g_lifting) {   // the raised point reached, or the lift's bound: the hold point goes back to the centre, the drive down through the box
            if (Dist3(h, g_holdT) <= kInsideOkCm || now - g_liftMs > kLiftBoundMs) {
                g_lifting = false;
                g_holdT.Z = g_centreZ;
                g_stillMs = now;
                g_reachedMs = 0;
            }
        } else if (!inBox && g_lifts < kMaxLifts && now - g_stillMs >= kStillMs) {   // still outside the box: no BeginOverlap will come
            ++g_lifts;
            g_lifting = true;
            g_liftMs = now;
            g_holdT.Z = g_centreZ + kLiftCm;
            g_reachedMs = 0;
            UE_LOGI("[CI-DRILL] client %s n=%d try %d: the held drive stood still outside the box for %llu ms; the hold point rises %.0f cm and "
                    "comes back (lift %d of %d)", ArmName(), g_r, g_t, U(kStillMs), static_cast<double>(kLiftCm), g_lifts, kMaxLifts);
        }
        if (g_lifting || Dist3(h, g_holdT) > kInsideOkCm) g_reachedMs = 0;
        else if (!g_reachedMs) g_reachedMs = now;
        if (g_reachedMs && now - g_reachedMs > kInsideWaitMs) {
            UE_LOGI("[CI-DRILL] client %s n=%d try %d: the held drive stood at the box's centre %llu ms and was not filed", ArmName(), g_r, g_t,
                    U(kInsideWaitMs));
            return HoldState::Ready;
        }
        if (now - g_startMs <= kHoldBoundMs) return HoldState::Working;
        std::snprintf(why, whyLen, "the held drive stayed %.0f cm off the box's centre and was never filed", static_cast<double>(Dist3(h, g_holdT)));
        return HoldState::Failed;
    }
    g_holdStable = Dist3(h, g_holdPrev) < kStableCm ? g_holdStable + 1 : 0;
    g_holdPrev = h;
    const float off = std::hypot(g_holdT.X - h.X, g_holdT.Y - h.Y);
    if (g_holdStable >= kStableTicks && off <= kHoldOkCm && h.Z > g_holdTop) {
        UE_LOGI("[CI-DRILL] client %s n=%d try %d: the held drive is over the basket (grabLen %.0f, %d correcting walk(s)): measured at "
                "(%.0f,%.0f,%.0f), %.1f cm from the hold point horizontally, %.0f cm above the rim; letting go", ArmName(), g_r, g_t,
                static_cast<double>(dist), g_corrections, static_cast<double>(h.X), static_cast<double>(h.Y), static_cast<double>(h.Z),
                static_cast<double>(off), static_cast<double>(h.Z - g_holdTop));
        UE_LOGI("[CI-DRILL] client %s n=%d try %d: held over the basket, before the release: R4 overlaps counted on this peer "
                "%llu", ArmName(), g_r, g_t, U(cen::R4ItemOverlaps() - g_heldAll0));
        return HoldState::Ready;
    }
    if (::GetTickCount64() - g_startMs <= kHoldBoundMs) return HoldState::Working;
    std::snprintf(why, whyLen, "the held drive stayed %.0f cm off the hold point, %.0f cm above the rim, %s", static_cast<double>(off),
                  static_cast<double>(h.Z - g_holdTop), g_holdStable >= kStableTicks ? "after it settled" : "and never settled");
    return HoldState::Failed;
}

void HoldReset() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_holdT = g_holdPrev = FVector{};
    g_holdTop = 0.f;
    g_holdStable = g_corrections = g_r = g_t = 0;
    g_inside = false;
    g_reachedMs = 0;
    g_heldAll0 = g_startMs = 0;
}

}  // namespace coop::dev::container_intent_drill::detail::into
