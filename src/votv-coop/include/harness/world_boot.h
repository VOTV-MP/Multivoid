// harness/world_boot.h -- WHICH gameplay world this process brings up, by whichever route, and
// what happens when one will not come up. Three routes, one concept: the story save this peer
// boots into, the world a joiner downloads from its host, and the world the host picker chose.
// All three block the TimelineThread on a level load and pump the game thread while they wait
// (harness/pump.h). Starting a SESSION is not this module's business; the picker's route ends in
// the lifecycle's own entry point (harness/session_runtime.h), so one place owns load-then-host.

#pragma once

namespace harness::world_boot {

// Boot story gameplay: LoadStorySave re-issues the open each tick while still at the splash or
// the menu (a single early open is dropped) and returns true once gameplay is reached; ~1.5 s
// between opens, blocking this worker until loaded or the ~120 s cap. `forceFresh` forces the
// blank New Game path (the menu-mode join's fallback); `slotOverride` loads that slot (the
// downloaded coop slot). `forceGameMode` carries the host's mode on both paths: the zcoop_ prefix
// matches no mode, and the blank New Game has no slot file to carry one at all.
bool BootStorySaveBlocking(bool forceFresh = false, const wchar_t* slotOverride = nullptr,
                           int forceGameMode = -1);

// The menu-mode join's world boot: wait for the save transfer (the session is already connecting
// at the menu), then load the downloaded slot. A failure at any step ends the join with a named
// reason and reopens the browser -- it does not fall back to a world of its own. It blocks the
// TimelineThread, so the play loop's abort branch cannot run meanwhile and the Cancel, the phase
// watchdog's abort and a dead session are drained here.
void DriveMenuModeJoinWorldBoot();

// The host-with-save orchestration: if one was queued (the picker's "Host selected save" or
// "New Game & Host"), load the chosen world or create the new save first, polling like
// BootStorySaveBlocking, then start the session. A host the player's ICE policy refuses
// (coop/net/ice_policy.h) is refused before the load, its lobby withdrawn. A no-op when nothing
// is queued; on the TimelineThread, where the blocking load and the start belong.
void DriveHostBootIfPending();

}  // namespace harness::world_boot
