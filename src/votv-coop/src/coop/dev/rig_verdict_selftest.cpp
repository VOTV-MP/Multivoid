// coop/dev/rig_verdict_selftest.cpp -- see coop/dev/rig_verdict_selftest.h.

#include "coop/dev/rig_verdict_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/rig_verdict.h"

#include <cstring>
#include <string>

namespace coop::dev::rig_verdict_selftest {

// Raised by the session at a milestone, so no tick reads it. Said once per connection life by rig_verdict's own
// latch, which DisconnectAll re-arms: a second `peer-world-ready` while a client stays (the join's double replay, a
// second slot) is dropped there, and one after the last client left is said again.
void OnMilestone(const char* milestone) {
    if (!milestone || std::strcmp(milestone, "peer-world-ready") != 0) return;
    const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::rig_verdict_selftest);
    using coop::rig_verdict::Outcome;
    const char* tag = "rig-verdict-selftest";
    const char* why = "the self-test knob asked for it";
    if (v == "pass") coop::rig_verdict::Say(tag, Outcome::Pass, "%s", why);
    else if (v == "fail") coop::rig_verdict::Say(tag, Outcome::Fail, "%s", why);
    else if (v == "invalid") coop::rig_verdict::Say(tag, Outcome::Invalid, "%s", why);
}

}  // namespace coop::dev::rig_verdict_selftest
