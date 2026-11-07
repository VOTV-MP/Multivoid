// coop/dev/prop_tool_park_selftest.h -- [dev] the order in which the prop-properties tool's received states wait for a
// held copy and replay (coop/props/prop_tool_park.h), checked on stated inputs and a fake engine side. Ini
// prop_props_park_selftest=1 (env VOTVCOOP_PROP_PROPS_PARK_SELFTEST), once per process, in the first tick of either peer.
// "prop_tool_park selftest: ALL PASS" is the --done-marker, "prop_tool_park selftest FAIL" the --fail-marker. Red arm:
// prop_props_park_selftest_break=1, whose first case leaves the older state standing, so the run must print FAIL.

#pragma once

namespace coop::dev::prop_tool_park_selftest {

// Game thread, per tick; a single bool read once it has run or when off.
void Tick();

}  // namespace coop::dev::prop_tool_park_selftest
