// coop/props/prop_drive_rules_selftest.cpp -- the boot selftest of the drive's turn measure; see coop/props/prop_drive_rules.h.

#include "coop/props/prop_drive_rules.h"

#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cmath>

namespace coop::prop_drive_rules {

namespace {

using R = ue_wrap::FRotator;

// The per-axis measure, the largest normalized difference of one axis: the reference TurnDeg is checked against. It reads
// the still pairs below as turns and TurnDeg does not, so the pairs are the cases a per-axis rule gets wrong.
float AxesDeg(const R& a, const R& b) {
    const float p = std::fabs(ue_wrap::NormalizeAxis(a.Pitch - b.Pitch));
    const float y = std::fabs(ue_wrap::NormalizeAxis(a.Yaw - b.Yaw));
    const float r = std::fabs(ue_wrap::NormalizeAxis(a.Roll - b.Roll));
    return std::max(p, std::max(y, r));
}

}  // namespace

bool RunTurnDegSelftest() {
    int pass = 0, total = 0;
    auto ok = [&](bool cond, const char* what) {
        ++total;
        if (cond) ++pass;
        else UE_LOGE("prop_drive_rules selftest: FAIL -- %s", what);
    };
    const ue_wrap::FVector origin{1.f, 2.f, 3.f};
    // At pitch -90 one orientation has every yaw and roll of the same sum; at +90, of the same difference. A still prop on
    // its side reads back with another pair from one read to the next: the pair reads as a turn to the per-axis measure and
    // as no turn to TurnDeg.
    auto still = [&](const R& a, const R& b, const char* what) {
        ok(AxesDeg(a, b) > kSendEpsDeg, what);
        ok(TurnDeg(a, b) < kSendEpsDeg, what);
    };
    still(R{-90.f, 30.f, 10.f}, R{-90.f, -20.f, 60.f}, "pitch -90, yaw and roll of one sum");
    still(R{-90.f, 30.f, 10.f}, R{-90.f, 50.f, -10.f}, "pitch -90, a second pair of one sum");
    still(R{90.f, 30.f, 10.f}, R{90.f, 50.f, 30.f}, "pitch +90, yaw and roll of one difference");
    ok(!MovedPastEps(origin, R{-90.f, 30.f, 10.f}, origin, R{-90.f, -20.f, 60.f}),
       "a still prop on its side read as moved past the send epsilons");
    // A real turn reads as its angle, on the flat and on the side.
    ok(std::fabs(TurnDeg(R{0.f, 0.f, 0.f}, R{0.f, 10.f, 0.f}) - 10.f) < 0.1f, "a 10 degree yaw did not read as 10 degrees");
    ok(std::fabs(TurnDeg(R{-90.f, 30.f, 10.f}, R{-90.f, 30.f, 15.f}) - 5.f) < 0.1f,
       "a 5 degree roll on its side did not read as 5 degrees");
    // The send epsilon's edge, through the rule the send check runs.
    ok(!MovedPastEps(origin, R{0.f, 0.4f, 0.f}, origin, R{0.f, 0.f, 0.f}), "a 0.4 degree turn read as moved");
    ok(MovedPastEps(origin, R{0.f, 0.6f, 0.f}, origin, R{0.f, 0.f, 0.f}), "a 0.6 degree turn did not read as moved");
    // The yaw wraps, an identical pose is no turn, and the same orientation as the negated quaternion (yaw 360) is none.
    ok(std::fabs(TurnDeg(R{0.f, 179.f, 0.f}, R{0.f, -179.f, 0.f}) - 2.f) < 0.01f, "yaw 179 and -179 did not read as 2 degrees");
    ok(TurnDeg(R{10.f, 20.f, 30.f}, R{10.f, 20.f, 30.f}) < 0.001f, "an identical pose read as a turn");
    ok(TurnDeg(R{0.f, 0.f, 0.f}, R{0.f, 360.f, 0.f}) < 0.001f, "yaw 0 and yaw 360 read as a turn");
    // The location axes, with no turn.
    ok(MovedPastEps({1.6f, 2.f, 3.f}, R{}, origin, R{}), "a 0.6 cm step on X did not read as moved");
    ok(!MovedPastEps({1.4f, 2.f, 3.f}, R{}, origin, R{}), "a 0.4 cm step on X read as moved");
    if (pass == total) UE_LOGI("prop_drive_rules selftest: %d of %d passed", pass, total);
    return pass == total;
}

}  // namespace coop::prop_drive_rules
