// coop/player/puppet_events_selftest.cpp -- the un-gated truth table of puppet_events::StepLife, run once per process
// (shape: coop/net/stream_slot_selftest.cpp). A join mid-death and a pose before the peer's world is ready are
// states no rig arm can hold still, so the life edges are proved here on pinned poses.

#include "coop/player/puppet_events.h"

#include "ue_wrap/core/log.h"

#include <cstdint>

namespace coop::puppet_events {
namespace {

bool RunChecks() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("puppet_events selftest FAIL: %s", what);
    };
    constexpr uint8_t kFull = 255, kZero = 0;

    LifeStep s = StepLife(Seen::Nothing, kFull, true);
    check(s.now == Seen::Alive && !s.raise, "an alive first pose of a ready slot holds alive and raises nothing");
    s = StepLife(Seen::Alive, kZero, true);
    check(s.now == Seen::Dead && s.raise && !s.alive, "alive then dead raises a death");
    s = StepLife(Seen::Dead, kZero, true);
    check(s.now == Seen::Dead && !s.raise, "a repeated dead pose raises nothing");
    s = StepLife(Seen::Dead, kFull, true);
    check(s.now == Seen::Alive && s.raise && s.alive, "dead then alive raises a revive");
    s = StepLife(Seen::Nothing, kZero, true);
    check(s.now == Seen::Dead && s.raise && !s.alive, "a dead first pose of a ready slot raises a death (joined dead)");
    s = StepLife(Seen::Nothing, kZero, false);
    check(s.now == Seen::Nothing && !s.raise, "a dead pose before the world is ready is held as unknown");
    s = StepLife(Seen::Nothing, kFull, false);
    check(s.now == Seen::Nothing && !s.raise, "an alive pose before the world is ready is held as unknown");
    s = StepLife(Seen::Alive, kZero, false);
    check(s.now == Seen::Alive && !s.raise, "a not-ready pose changes no held state");
    s = StepLife(Seen::Nothing, 1, true);
    check(s.now == Seen::Alive && !s.raise, "the smallest health is alive");

    if (pass == total) {
        UE_LOGI("puppet_events selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("puppet_events selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace

// Its caller is the subsystem install pump, which runs every tick, and the checks are pure: the first run's answer is
// every later one's, so it is run once and kept (it was 61 lines a second, four in five of a session's log).
bool RunSelftest() {
    static const bool s_passed = RunChecks();
    return s_passed;
}

}  // namespace coop::puppet_events
