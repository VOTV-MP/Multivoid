// coop/dev/dev_lanes.h -- the developer drills, probes and selftests, wired in one place. Each entry runs
// where the session's pump called these before they moved here, in the same order, because some of them
// read what the pump around them has just done: a probe's verdict prints before the state it describes
// is reset, and a drill's leg reads the lanes that ticked ahead of it. Every one is off unless its
// [dev] knob is set, and costs a single read when off. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::dev_lanes {

// At the session's install, after the garbage lane and before the host spawn watcher.
void Install(coop::net::Session& session);

// At the session's end, before the prop seams reset: the probes' verdicts first, then each drill back
// to its first step.
void EndSession();

// At the session's end, after the prop lanes reset: the selftests re-arm for the next session.
void RearmSelftests();

// In the gameplay tick, after the host publishes its driven props: the drills' and censuses' legs.
void TickDrills(coop::net::Session& session);

// In the gameplay tick, after the hotbar icons: the live store readout and the pickup drill, each in its
// own perf bucket.
void TickInventory(coop::net::Session& session);

// At the gameplay tick's end: the RE probes, each installing itself when its knob is set.
void TickProbes(coop::net::Session& session, bool isConnected, bool isHost);

}  // namespace coop::dev::dev_lanes
