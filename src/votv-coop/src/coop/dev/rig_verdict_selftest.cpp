// coop/dev/rig_verdict_selftest.cpp -- see coop/dev/rig_verdict_selftest.h.

#include "coop/dev/rig_verdict_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/rig_verdict.h"

#include <cstring>
#include <string>

namespace coop::dev::rig_verdict_selftest {

// Raised by the session at a milestone, so no tick reads it. Said once per session by rig_verdict's own latch: a
// rejoin's second `peer-world-ready` is dropped there.
void OnMilestone(const char* milestone) {
    if (!milestone || std::strcmp(milestone, "peer-world-ready") != 0) return;
    const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::rig_verdict_selftest);
    using coop::rig_verdict::Outcome;
    if (v == "pass") coop::rig_verdict::Say("rig-verdict-selftest", Outcome::Pass, "the self-test knob asked for it");
    else if (v == "fail") coop::rig_verdict::Say("rig-verdict-selftest", Outcome::Fail, "the self-test knob asked for it");
    else if (v == "invalid")
        coop::rig_verdict::Say("rig-verdict-selftest", Outcome::Invalid, "the self-test knob asked for it");
}

}  // namespace coop::dev::rig_verdict_selftest
