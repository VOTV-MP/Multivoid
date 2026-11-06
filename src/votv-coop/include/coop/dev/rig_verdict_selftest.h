// coop/dev/rig_verdict_selftest.h -- [dev] rig_verdict_selftest: a drill-free way to show the rig each ending.
//
// The rig ends a run on `rig: VERDICT <tag> PASS|FAIL|INVALID` (coop/session/rig_verdict.h). A gate nothing can
// show failing is a gate nobody can trust, so with rig_verdict_selftest=pass|fail|invalid the host says that
// verdict, tag `rig-verdict-selftest`, once per session when a client's world is ready (the milestone
// `peer-world-ready` of coop/session/rig_ready.h, which calls OnMilestone). Off by default; a dev instrument.

#pragma once

namespace coop::dev::rig_verdict_selftest {

// Called with every milestone the rig is told; says the verdict on `peer-world-ready` when the knob is set. Game thread.
void OnMilestone(const char* milestone);

}  // namespace coop::dev::rig_verdict_selftest
