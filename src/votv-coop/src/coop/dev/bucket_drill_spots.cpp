// coop/dev/bucket_drill_spots.cpp -- the places the bucket drill's stage offers the game's own place to set a bucket down: the table of
// candidate places in the anchor's frame, and what a candidate must pass before it is offered (clear of the buckets already set down,
// a floor under it). The game's own place test judges the rest. See bucket_drill_stage.cpp for the carry itself.

#include "bucket_drill_detail.h"

#include "coop/element/intent_authority.h"
#include "coop/props/prop_act_host.h"

#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>

namespace coop::dev::bucket_drill::detail {

namespace E = ue_wrap::engine;

namespace {

// The places a bucket may be set down, in the anchor's frame: `fwd` cm before it, `side` cm to its right, from 90 to 340 cm. The host's
// hold judge measures a client's reach from the puppet's origin to the bucket's colliding bounds as kActReachUU + the bounds' radius +
// kPoseStalenessUU (intent_authority.cpp ReachVerdict; HoldFits itself has no distance term), at least 800 cm, so every place is inside
// it. A client acts on a bucket from where its own walk leaves it (bucket_drill_walk.cpp), so no place is chosen for the client's reach.
// A place 100 cm straight before the puppet overlapped the puppet (the game's place test named mainPlayer_C), so the nearest places
// here may be refused by the game's own test.
struct Spot { float fwd, side; };
// The two places at (28, +-84) stand on the ring of the first, 88 cm from the anchor and 104 cm from {90, 0}: a second bucket there is
// clear of the first. The places from {0, +-120} on lie beside the anchor and far before it, for a stage whose nearer places are taken.
constexpr Spot kSpots[] = {{90, 0},    {28, 84},    {28, -84},   {110, 0},    {90, 70},    {90, -70},   {120, 0},    {130, 0},
                           {110, 75},  {110, -75},  {140, 0},    {120, 75},   {120, -75},  {130, 75},   {130, -75},  {150, 0},
                           {150, 80},  {150, -80},  {180, 0},    {180, 80},   {180, -80},  {210, 0},    {150, 160},  {150, -160},
                           {210, 80},  {210, -80},  {180, 160},  {180, -160}, {210, 160},  {210, -160}, {0, 120},    {0, -120},
                           {240, 0},   {240, 80},   {240, -80},  {270, 0},    {270, 80},   {270, -80},  {240, 160},  {240, -160},
                           {300, 0},   {270, 160},  {270, -160}, {300, 80},   {300, -80},  {300, 160},  {300, -160}};
static_assert(static_cast<int>(sizeof(kSpots) / sizeof(kSpots[0])) == kSpotCount, "kSpotCount is the table's size");
constexpr float kPlaceDropCm = 60.f;       // a body's height below the anchor's centre

constexpr bool SpotsInJudgeReach() {
    const float reach = coop::prop_act_host::kActReachUU + coop::element::kPoseStalenessUU;
    for (const Spot& k : kSpots)
        if (k.fwd * k.fwd + k.side * k.side + kPlaceDropCm * kPlaceDropCm > reach * reach) return false;
    return true;
}
static_assert(SpotsInJudgeReach(), "every place a bucket is set down lies inside the host's hold judge's reach");

// The world place of `k` for `anchor`.
bool SpotAt(void* anchor, const Spot& k, ue_wrap::FVector& at) {
    ue_wrap::FVector p{};
    if (!E::TryGetActorLocation(anchor, p)) return false;
    const ue_wrap::FVector f = E::GetActorForwardVector(anchor);
    at = {p.X + f.X * k.fwd - f.Y * k.side, p.Y + f.Y * k.fwd + f.X * k.side, p.Z - kPlaceDropCm};
    return true;
}

constexpr float kFloorAboveCm = 30.f;   // the floor under a place is traced from this far above it
constexpr float kFloorBelowCm = 200.f;  // and to this far below it

}  // namespace

// True when an item stood `fwd` cm before the anchor and `side` cm to its right is clear of every place.
bool ItemClearOfSpots(float fwd, float side) {
    for (const Spot& k : kSpots)
        if (std::hypot(k.fwd - fwd, k.side - side) < kClearOfBucketCm) return false;
    return true;
}

bool OfferSpots(const OfferIn& in, Offer& out) {
    out = Offer{};
    void* const me = Local();
    if (!in.anchor || !me) return false;
    for (int k = 0; k < kSpotCount; ++k) {
        ue_wrap::FVector at{};
        if (!SpotAt(in.anchor, kSpots[k], at)) return false;
        bool clear = true;
        for (int j = 0; clear && j < in.nPlaced; ++j) clear = std::hypot(in.placed[j].X - at.X, in.placed[j].Y - at.Y) >= kClearOfBucketCm;
        if (!clear) {
            ++out.crowded;
            continue;
        }
        // The level geometry the bucket stands on: a physics body (a bucket, a prop) and the players are not in the trace's set.
        const int floor = ue_wrap::trace::LineBlockedStatDyn(me, {at.X, at.Y, at.Z + kFloorAboveCm}, {at.X, at.Y, at.Z - kFloorBelowCm});
        if (floor < 0) {
            out.broken = "the floor under a place could not be traced";
            return true;
        }
        if (floor == 0) {
            ++out.noFloor;
            continue;
        }
        out.spots[out.n++] = at;
    }
    return true;
}

}  // namespace coop::dev::bucket_drill::detail
