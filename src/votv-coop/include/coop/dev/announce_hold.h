// coop/dev/announce_hold.h -- holds this client's ClientWorldReady at its last gate (the pre-announce drill arms).
//
// The pump sends the announce once the world is up and its gates are clear; while this hold is set the send waits,
// every gate untouched. It is the SENDER's switch: nothing on the host reads it, so a host judges a take or use that
// arrives before the announce exactly as it would from a slow joiner. Set by the hostile drill's act_notyet and
// act_flood_early arms (hostile_drill_act), which release it after their requests are sent. Dev code; the hold is
// off until a drill sets it.

#pragma once

namespace coop::dev::announce_hold {

void Set(bool on);   // an atomic bool
bool Held();

}  // namespace coop::dev::announce_hold
