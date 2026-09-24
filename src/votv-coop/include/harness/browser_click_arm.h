// harness/browser_click_arm.h -- TEST-ONLY: the VOTVCOOP_* variables that do what a player's click in the
// multiplayer menu does, so an autonomous run can open the browser, host and join without a mouse. They
// fire once from the harness timeline, once what a click finds ready is loaded: the master list, the
// session manager's configuration and the durable identity (a real click comes later still, once the menu
// is up). A host announce made before the identity has loaded reaches the master empty and is refused
// ("missing/bad identity"). Each is inert unless set.
//
//   BROWSER_OPEN=1           the MULTIPLAYER button: the update check and the thanks list go to the
//                            chosen master, as a click's do
//   TEST_CONNECT_DIRECT=h:p  the browser's direct connect
//   TEST_HOST_LOBBY=1        the test-only HostLobby primitive, which no button runs
//   TEST_JOIN_LOBBY=<id>     a row's Connect through the chosen master, without the row's version check
//   TEST_HOST_SAVE=<slot>    HostWithSave on an existing save, what the session window's Host runs
//   TEST_HOST_NEW=<name>     the same on a new story game

#pragma once

namespace harness::browser_click_arm {

// Read the variables once and fire what they name. Called once, from the timeline.
void FireFromEnv();

}  // namespace harness::browser_click_arm
