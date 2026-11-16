// coop/creatures/served_player.h -- a robot the host runs for a client answers its player-0 reads with that
// client's puppet (HOST).
//
// The kerfurs' brains run on the host alone and read "the player" as player 0, the host. A lane that knows
// whom its robot serves -- kerfus_follow: whoever turned it on; kerfur_command: whoever gave the command --
// records it here with the seams its robot reads through, and those reads answer that player's puppet: the
// plain kerfur's GetPlayerCharacter(0) and GetPlayerPawn(0), the Omega's GetPlayerPawn(0) and the disc
// insert's lib.getMainPlayer(), told at the script-body gate by the variable the insert writes; the natives
// by post-hooks whose source is the calling robot. Each seam is armed only while a record needs it, and a
// robot is recorded only while all its seams can answer (which read is which: docs/coop-dispatch-visibility.md).
// A walking mannequin (coop/creatures/walker_target) and a hunter the host's creature rows name
// (coop/creatures/hunt_target) are served the same way; a hunter's rows add two seams, getMainPlayer and
// GetPlayerCameraManager (a puppet has no camera manager, so that read answers the body).
// This module writes nothing on the puppet: its hand is written only by kerfur_disc_carry, for a get_reports
// that carries the client's disc. Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::served_player {

// The player-0 reads a robot's Blueprint makes, each through a seam of its own.
enum Seam : uint8_t {
    kPlayerCharacter = 1 << 0,  // GameplayStatics::GetPlayerCharacter, a native post-hook
    kPlayerPawn      = 1 << 1,  // GameplayStatics::GetPlayerPawn, a native post-hook
    kMainPlayer      = 1 << 2,  // the Omega's disc-insert lib_C::getMainPlayer calls, a script-body gate watch
    kMainPlayerAny   = 1 << 3,  // every lib_C::getMainPlayer call the creature makes (a hunter's: its ragdoll test, its hit)
    kCameraManager   = 1 << 4,  // GameplayStatics::GetPlayerCameraManager, a native post-hook, answered with the body
};

// On a host, install the two post-hooks disarmed and resolve the gate watch once, retiring it until a record
// needs it. Idempotent; retried until each settles, and a seam this build lacks is said once and not asked
// again. Game thread.
void Install(coop::net::Session* session);

// HOST: whether every seam in `seams` can answer now (the hooks installed, the watch resolved), and whether
// one of them never will, refused for this process. A lane holds a request while the first is false and the
// second is not. Game thread.
bool SeamsReady(uint8_t seams);
bool SeamsRefused(uint8_t seams);

// HOST: from now on `robot`'s player-0 reads through `seams` answer `slot`'s puppet (a slot of 0 is the
// host itself: the record goes). False, with no record, while one of those seams is not ready: the robot
// then reads player 0 through all of them. Game thread.
bool Serve(void* robot, uint8_t slot, uint8_t seams);

// HOST: `robot`'s reads answer player 0 again. Game thread.
void Unserve(void* robot);

// The slot a live `robot` serves, or 0 (the host, no record, or a dead robot's).
uint8_t ServedSlot(void* robot);

// How many player-0 reads this process has answered with a served client (all seams): what a drill compares to show that a
// creature's Blueprint really asked through a seam. Game thread.
unsigned long long AnsweredCount();

// Drop dead robots' records and arm each seam while a record needs it. Every pump tick; nothing to do on a
// client. Game thread.
void Tick();

// A peer left: every robot it was served goes back to player 0.
void OnPeerLeft(uint8_t slot);

// Session end: every record goes and every seam is disarmed.
void OnDisconnect();

}  // namespace coop::served_player
