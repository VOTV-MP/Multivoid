// coop/dev/pr64_drill.h -- [dev] the pr64 drill: a press of E, as a player makes it, on what the host owns, and both
// peers print what they saw and a verdict. The reds (pr64_record_window_red_ms, pr64_coin_local_credit_red) put back what
// the PR removed, and the arm it breaks must print its client FAIL line.
//   coin -- HOST spawns a coin gun and a prop on a floor point a nav route reaches, mints one coin through the gun's
//           own sell verb, names its element id and sends the place where it came to rest. CLIENT walks to it with the
//           director, aims, presses E through the game window, and counts the credits it makes itself with a watch of
//           lib_C::addPoints. The client passes with none, the coin gone and its balance equal to the host's; the host
//           passes when its balance rose by the coin's points and the coin is gone.
//   lid  -- HOST spawns a drive box in front of its own player with a drive in slot 0, aims at the box and holds E until
//           it opens (change A), then aims at the slot and holds E until the drive comes out (change B). The host
//           says the box's (opened, drive0) pair; the CLIENT reads its own copy for up to 2 s and says whether the
//           two boxes agree. A posted mouse button never reaches the game, so every change is a held E.
// The arms are the values of the pr64_drill config row. Lines are tagged [PR64-DRILL]; each peer prints
// `<role> <arm> PASS` or `<role> <arm> FAIL: <why>`, then `<role> DONE`.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::pr64_drill {

bool IsEnabled();

// Game thread, once per pump tick; a latched read when off. The walk is the director's own worker thread.
void Tick(coop::net::Session* session);

// The arm's steps belong to one session; a finished drill stays finished.
void OnDisconnect();

}  // namespace coop::dev::pr64_drill
