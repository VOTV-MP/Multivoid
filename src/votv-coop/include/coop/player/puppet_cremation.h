// coop/player/puppet_cremation.h -- a peer's puppet is cremated by its owner, never by this machine's cremator.
//
// A cremator that collects a puppet drains its burn-health and, at 0, destroys the puppet's actor; the next
// remote pose then spawns a fresh puppet. The cremator asks each collected actor that implements int_objects
// `crematorAction(&action)` first ("do you handle your own cremation?"); a puppet answers true, so the cremator's
// tick returns at it and nothing is destroyed. Installed beside coop/player/player_damage, which refuses a hit
// and a fire on a puppet for the same reason.

#pragma once

namespace coop::puppet_cremation {

// Watch `mainPlayer_C::crematorAction` by name; idempotent, game thread. Needs no session: a puppet answers
// whatever the role.
void Install();

}  // namespace coop::puppet_cremation
