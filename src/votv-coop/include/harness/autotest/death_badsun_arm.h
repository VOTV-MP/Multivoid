// harness/autotest/death_badsun_arm.h -- the death drill's Bad Sun arm (VOTVCOOP_DEATH_BADSUN=1): the
// revive must wake a player somewhere the game's own Bad Sun event cannot kill again.
//
// The HOST walks to the KPP (open sky) with the bot director, then starts the event the way the Bad Sun
// drill's `spawn` arm does (coop/dev/badsun_drill_lane.cpp: the clock to 08:00, then the gamemode's own
// Spawn Bad Sun). The event's damage tick kills the standing player; the arm waits for that death and
// grades the 90 s after it:
//   PASS         the revive's target kind is `bed` and no second death came in the 90 s;
//   FAIL         the target was the KPP fallback, or the player died again;
//   INCONCLUSIVE a precondition failed (a client role, a Bad Sun already live, no walk to the KPP, the
//                event never killed the player).
// With VOTVCOOP_DEATH_NO_BED=1 the bed lookup finds nothing, the revive wakes the player at the KPP and
// the event kills it again: that run must end FAIL, which is this arm's red.
// Ends on `death_test: BADSUN VERDICT <word>` and then `death_test: DONE`.

#pragma once

namespace harness::autotest {

// Runs on the drill's worker thread, after the death drill's readiness wait.
void RunDeathBadSunArm(bool isClient);

}  // namespace harness::autotest
