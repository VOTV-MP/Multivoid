// coop/dev/grid_drill.h -- [dev] the power grid across both peers: the panel's presses, the generators' rows and
// their puzzles. Each client press waits for the host's answer to leave the lever flipped.
//   run  -- the client walks to the base panel by the director and presses its light lever; the host breaks the
//           drill's generator (the one nearest the panel) after the first press. After the second the client presses
//           its Activate button from where it stands, which the host rolls back beyond its reach (the generators stand
//           470-620 m out); after a third the host repairs it, with a turn-on the client runs too.
//   puzzle -- as run up to the break; the client's panel must hold the host's puzzle, the host's own inputs after the
//           second press must reach it, and the client's own input from out of reach must roll back.
//   puzzlesolve -- the client walks beside the drill's generator by the director; the host breaks it, and the
//           client solves its panel and presses Activate. puzzleflood: from there, a burst of inputs, taken at a rate.
//   upgrade[red], upgraderepair[red|lossred] -- installs the host hands the client, and a repair after one (grid_drill_upgrade.h).
//   join -- the host breaks the drill's generator as it hosts; the joiner must find the blackout and its puzzle.
//   lockout, lockjoin -- the desk virus's lockout, from the client's world-ready or during its join, 60 s long.
//   red, puzzlered -- a client applies the host's canonical and rows raw, or never writes a puzzle: the controls.
// Each peer says its grid on "[GRID-DRILL]" lines; the client's DONE line, its copy equal to the host's, ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::grid_drill {

// Game thread, once per pump tick; a latched read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again.
void OnDisconnect();

}  // namespace coop::dev::grid_drill
