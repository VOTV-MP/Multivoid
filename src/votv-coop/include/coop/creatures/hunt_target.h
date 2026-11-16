// coop/creatures/hunt_target.h -- the host's hunters go for the nearest player, within their own range.
//
// The host runs every shared creature's brain, and a Blueprint that reads "the player" as player 0 (GetPlayerPawn(0),
// GetPlayerCharacter(0), GetPlayerCameraManager(0), lib.getMainPlayer()) hunts the host alone. Each row names such a class, the
// player-0 reads it makes (coop/creatures/served_player's seams) and the range its own Blueprint tests (0: none). A pass every
// quarter second takes each live creature of a row and asks the rule (coop/creatures/hunt_target_rule.h: keep a target until it
// is dead, gone or out of range, else the nearest living player in range, else the host) which slot it hunts; a client's slot is
// recorded in served_player, so the creature's own reads answer that client's puppet. The host is no record, so a creature
// hunting it reads what it always read. The pass, not the read, is where the target changes: the Blueprints read the player in
// many places and none is a decision point. Creatures that re-pick by distance every tick (the zombie, the fossilhound, the
// Killer Wisp: coop/creatures/wisp_attack_sync) already see puppets and are not rows. Hits land through the existing relays
// (coop/player/player_damage, coop/creatures/creature_hit); the Grays' punch is coop/creatures/hunt_gray's.
// VOTVCOOP_HUNT_SKIP_HOST=1 (row hunt_skip_host) leaves the host out of the candidates, so a drill's hunter must pick a client.
// Coop layer; game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::hunt_target {

// Host only: nothing to install (the seams are served_player's), kept so the lanes call one entry per module. Idempotent.
void Install(coop::net::Session& session);

// Per gameplay tick on the HOST: the quarter-second pass over the live creatures of the rows. Game thread.
void Tick(coop::net::Session& session);

// The slot `creature` hunts through a row (0 the host, a client's slot a served record), kNoTarget for a creature no row holds
// or one not yet chosen for. Game thread.
uint8_t TargetOf(void* creature);

// The slot of the nearest living player to `origin` (an actor) within `rangeCm` (0: any), by the same rule a hunter's pass asks and
// with the dev switch honoured: a client's slot, or the host's (0) when no client is nearer, none is in range or `origin` has no
// position. For a consumer that is not a creature of a row and chooses once (coop/creatures/hunt_gray). Game thread.
uint8_t NearestTo(void* origin, float rangeCm);

// Whether the dev switch hunt_skip_host (VOTVCOOP_HUNT_SKIP_HOST=1) is set: the host is left out of every candidate list. Read once.
bool SkipHost();

// A peer left: every creature that hunted `slot` chooses again at the next pass.
void OnPeerLeft(uint8_t slot);

// The session ended: the held creatures are forgotten.
void OnDisconnect();

}  // namespace coop::hunt_target
