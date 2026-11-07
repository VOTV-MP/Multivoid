// coop/creatures/walker_target_rule_selftest.cpp -- the walker's target rule on stated inputs (coop/creatures/walker_target_rule.h).
//
// No honest run exercises these rows: the walker's Blueprint reads its players only where its target is lost or reached, so a run
// never reads them with another player nearer (measured, the walker drill has no keep step), and a wrong rule reads as a walker
// that hunts someone. Each row names its players and what the walker must do.

#include "coop/creatures/walker_target_rule.h"

#include "ue_wrap/core/log.h"

#include <cstddef>
#include <cstdint>

namespace coop::walker_target {

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("walker_target selftest FAIL: %s", what);
    };
    auto decide = [](uint8_t current, const PlayerView (&p)[3]) { return DecideTarget(current, p, 3); };
    constexpr uint8_t H = kHostSlot;
    constexpr float R = kRangeCm;

    // KEEP: another player nearer does not take the walker over, whoever the target is.
    {
        const PlayerView p[3] = {{0, true, 15000.f}, {1, true, 3000.f}, {2, true, 9000.f}};
        check(decide(H, p) == H, "the host stays the target while a client stands nearer");
        check(decide(2, p) == 2, "a client stays the target while another client and the host stand nearer");
    }
    {
        const PlayerView p[3] = {{0, true, 1000.f}, {1, true, 15000.f}, {2, false, 0.f}};
        check(decide(1, p) == 1, "a client stays the target while the host stands nearer");
    }
    // KEEP at the range's edge: the Blueprint's own test is "farther than", so the range itself is in.
    {
        const PlayerView p[3] = {{0, true, 500.f}, {1, true, R}, {2, true, 800.f}};
        check(decide(1, p) == 1, "a target exactly at the range is kept");
    }

    // LOSE: a target that is not alive is dropped and the nearest living player taken.
    {
        const PlayerView p[3] = {{0, true, 9000.f}, {1, false, 100.f}, {2, true, 4000.f}};
        check(decide(1, p) == 2, "a client that is not alive is lost and the nearest living player taken");
    }
    {
        const PlayerView p[3] = {{0, true, 5000.f}, {1, false, 100.f}, {2, false, 50.f}};
        check(decide(1, p) == H, "a client that is not alive is lost and, with no other living player, the host taken");
    }
    // LOSE: a target beyond the range at the read.
    {
        const PlayerView p[3] = {{0, true, R + 5000.f}, {1, true, R + 1.f}, {2, true, 10000.f}};
        check(decide(1, p) == 2, "a client beyond the range is lost and the nearest player within it taken");
    }
    // LOSE: a client whose body did not read; the host is kept on the same read.
    {
        const PlayerView p[3] = {{0, true, 6000.f}, {1, true, -1.f}, {2, true, 7000.f}};
        check(decide(1, p) == H, "a client with no body is lost and the nearest player within the range taken");
    }
    {
        const PlayerView p[3] = {{0, true, -1.f}, {1, true, 2000.f}, {2, true, 3000.f}};
        check(decide(H, p) == H, "the host stays the target when its distance did not read, though a client stands nearer");
    }
    // LOSE: the host beyond the range is lost like any target; a player in range takes it.
    {
        const PlayerView p[3] = {{0, true, R + 100.f}, {1, true, 18000.f}, {2, false, 0.f}};
        check(decide(H, p) == 1, "the host beyond the range is lost and a client within it taken");
    }

    // ACQUIRE: with no target, the nearest living player within the range; the earlier slot on a tie.
    {
        const PlayerView p[3] = {{0, true, 9000.f}, {1, true, 4000.f}, {2, true, 7000.f}};
        check(decide(kNoTarget, p) == 1, "with no target the nearest living player is taken");
    }
    {
        const PlayerView p[3] = {{0, true, 4000.f}, {1, true, 4000.f}, {2, true, 6000.f}};
        check(decide(kNoTarget, p) == H, "on a tie the earlier slot is taken");
    }
    {
        const PlayerView p[3] = {{0, true, R + 10.f}, {1, false, 100.f}, {2, true, 12000.f}};
        check(decide(kNoTarget, p) == 2, "a player that is not alive is not taken, though it stands nearest");
    }
    {
        const PlayerView p[3] = {{0, true, R + 10.f}, {1, true, -1.f}, {2, true, 12000.f}};
        check(decide(kNoTarget, p) == 2, "a player whose body did not read is not taken");
    }
    {
        const PlayerView p[3] = {{0, true, R + 10.f}, {1, true, 12000.f}, {2, true, R + 1.f}};
        check(decide(kNoTarget, p) == 1, "a player beyond the range is not taken while another is within it");
    }
    // FALL BACK: with none in range, the host, so the Blueprint's own out-of-range branch waits.
    {
        const PlayerView p[3] = {{0, true, R + 1.f}, {1, true, R + 2.f}, {2, true, 63000.f}};
        check(decide(kNoTarget, p) == H, "with none in range the host is the target");
        check(decide(2, p) == H, "a target lost to the range, with none else in range, gives the host");
    }
    check(DecideTarget(kNoTarget, nullptr, 0) == H, "with no players listed the host is the target");
    // A target no longer listed (its slot gone) is lost.
    {
        const PlayerView p[2] = {{0, true, 9000.f}, {2, true, 7000.f}};
        check(DecideTarget(1, p, 2) == 2, "a target with no entry is lost and the nearest player taken");
    }

    if (pass == total) {
        UE_LOGI("walker_target selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("walker_target selftest FAIL: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::walker_target
