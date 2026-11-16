// coop/creatures/hunt_target_rule_selftest.cpp -- the hunters' target rule on stated inputs (coop/creatures/hunt_target_rule.h).
//
// No honest run exercises these rows: the two players of a drill stand at one fixed spot, so "the client is nearer" cannot be staged
// by moving anyone, and a wrong rule reads as a creature that hunts someone. Each row names its players, the creature's range and
// what the creature must do. The mannequin's rows (walker_target_rule_selftest.cpp) run the same function through its wrapper.
// Run from the mannequin's selftest pass (walker_target::RunSelftest), the one boot pass that exists for target rules.

#include "coop/creatures/hunt_target_rule.h"

#include "ue_wrap/core/log.h"

#include <cstddef>
#include <cstdint>

namespace coop::hunt_target {

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("hunt_target selftest FAIL: %s", what);
    };
    constexpr uint8_t H = kHostSlot;
    constexpr float   R = 5000.f;  // a creature with a range (the Krampus's leash)
    const Rule ranged{R, false}, unbound{0.f, false}, ranged_skip{R, true}, unbound_skip{0.f, true};
    auto decide = [](uint8_t current, const PlayerView (&p)[3], Rule rule) { return Decide(current, p, 3, rule); };

    // ACQUIRE: a far player is never picked; the nearest one in range is.
    {
        const PlayerView p[3] = {{0, true, 9000.f}, {1, true, R + 1.f}, {2, true, 12000.f}};
        check(decide(kNoTarget, p, ranged) == H, "nobody in range: the host is the fallback, the far client is not picked");
    }
    {
        const PlayerView p[3] = {{0, true, 4000.f}, {1, true, 1500.f}, {2, true, 3000.f}};
        check(decide(kNoTarget, p, ranged) == 1, "the nearest player in range is picked, not the host");
    }
    {
        const PlayerView p[3] = {{0, true, 4000.f}, {1, true, 4000.f}, {2, true, 6000.f}};
        check(decide(kNoTarget, p, ranged) == H, "on a tie the earlier slot is picked");
    }
    {
        const PlayerView p[3] = {{0, true, R}, {1, true, R + 0.5f}, {2, false, 10.f}};
        check(decide(kNoTarget, p, ranged) == H, "a player exactly at the range is in; a dead one is not picked, though it stands nearest");
    }
    {
        const PlayerView p[3] = {{0, true, 30000.f}, {1, true, 20000.f}, {2, true, 40000.f}};
        check(decide(kNoTarget, p, unbound) == 1, "a creature with no range of its own takes the nearest player, however far");
    }
    {
        const PlayerView p[3] = {{0, true, 800.f}, {1, true, -1.f}, {2, true, 5000.f}};
        check(decide(kNoTarget, p, ranged) == H, "a player whose body did not read is not picked");
    }

    // KEEP: a nearer player does not take over; a kept host is kept without a measure.
    {
        const PlayerView p[3] = {{0, true, 4900.f}, {1, true, 200.f}, {2, true, 300.f}};
        check(decide(H, p, ranged) == H, "the host stays the target while a client stands nearer");
        check(decide(2, p, ranged) == 2, "a client in range is kept while another stands nearer");
    }
    {
        const PlayerView p[3] = {{0, true, -1.f}, {1, true, 200.f}, {2, true, 300.f}};
        check(decide(H, p, ranged) == H, "the host's unread body keeps it, as the Blueprint's own test is the judge");
    }

    // LOSE: death, range, no body, and a slot that is gone.
    {
        const PlayerView p[3] = {{0, true, 6000.f}, {1, false, 100.f}, {2, true, 3000.f}};
        check(decide(1, p, ranged) == 2, "a dead target is lost and the nearest living player taken");
    }
    {
        const PlayerView p[3] = {{0, true, 6000.f}, {1, true, R + 1.f}, {2, true, 5200.f}};
        check(decide(1, p, ranged) == H, "a target beyond the range is lost and, with none else in range, the host taken");
    }
    {
        const PlayerView p[3] = {{0, true, 900.f}, {1, true, -1.f}, {2, true, 7000.f}};
        check(decide(1, p, ranged) == H, "a client whose body did not read is lost");
    }
    {
        const PlayerView p[2] = {{0, true, 9000.f}, {2, true, 7000.f}};
        check(Decide(1, p, 2, unbound) == 2, "a target whose slot is gone is lost and the nearest player taken");
    }
    check(Decide(kNoTarget, nullptr, 0, ranged) == H, "with no players listed the host is the fallback");

    // THE DEV SWITCH: the host is out of the candidates and out of the keep; it stays the fallback.
    {
        const PlayerView p[3] = {{0, true, 100.f}, {1, true, 3000.f}, {2, true, 4000.f}};
        check(decide(kNoTarget, p, ranged_skip) == 1, "with the host left out the nearest client is picked though the host stands nearer");
        check(decide(H, p, ranged_skip) == 1, "with the host left out a creature that held the host lets it go");
    }
    {
        const PlayerView p[3] = {{0, true, 100.f}, {1, true, R + 1.f}, {2, false, 50.f}};
        check(decide(kNoTarget, p, ranged_skip) == H, "with the host left out and no client in range the host is the fallback");
    }
    {
        const PlayerView p[3] = {{0, true, 10.f}, {1, true, 90000.f}, {2, true, 95000.f}};
        check(decide(kNoTarget, p, unbound_skip) == 1, "with the host left out a creature with no range takes the nearest client");
    }

    if (pass == total) {
        UE_LOGI("hunt_target selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("hunt_target selftest FAIL: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::hunt_target
