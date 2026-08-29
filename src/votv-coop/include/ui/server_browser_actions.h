// ui/server_browser_actions.h -- the native server browser's action bar.
//
// CONNECT, HOST and REFRESH: the three controls the browser has been missing since it was
// built. Until now the native screen could show servers and select one, and then do nothing
// with either -- `docs/MULTIPLAYER_UI.md` recorded that as "selection exists, nothing
// consumes it", and HOST as "a missing LINK, not a missing button", because
// `ui/host_window_native` shipped with no way in but a dev flag.
//
// WHY A SEPARATE TU. `server_browser_native.cpp` is past the 800-LOC soft cap, and the
// modular rule's extraction trigger says new code that is conceptually its own subsystem
// goes in its own file rather than growing that one further. An action bar qualifies: it
// owns three buttons, their placement, their click routing and the sentences they produce,
// and it holds no row state and no screen state. (The row model is a second, larger
// extraction that is genuinely owed and is scheduled as plan step T6, where the choice of
// row model is made -- doing it now would refactor code that step is about to reshape.)
//
// PRINCIPLE 7: this is presentation. It authors no hosting and no joining of its own -- it
// calls the same `coop::session_manager` entry points the ImGui browser calls, so the two
// surfaces cannot drift into two different meanings of "Connect".
//
// Game thread only, like every other native-screen TU.

#pragma once

namespace ui::server_browser_actions {

// Build CONNECT / HOST / REFRESH into `footRow` (the footer HorizontalBox), styled from
// `donorBtn` (the game's own `ui_saveSlots_C.button_back`, so they carry its press and
// hover sounds). Call once per screen build, AFTER the status text, so the status's fill
// weight pushes these to the right edge -- Back at the left and actions at the right is
// what every native VOTV window does (docs/VOTV_UI_STYLE.md section 5).
//
// Returns false if any button could not be built; the caller treats that as a build
// failure rather than shipping a footer with a hole in it.
bool Build(void* footRow, void* donorBtn);

// Handle a left-button RELEASE while the browser is open. Returns true if one of these
// buttons was under the cursor and consumed the click, so the caller stops -- in
// particular, so a click on CONNECT is not ALSO read as a click on the row behind it.
//
// The release edge, not the press, for the reason the chrome uses it: these are real
// UButtons, so the press drives Slate's own pressed visual and acting on the down edge
// would tear the screen away from a button that never saw its own release.
bool OnReleaseEdge();

// Drop every widget pointer. Called when the menu instance dies, exactly like the browser's
// own reset -- the widgets died with it and holding them across instances is how a stale
// pointer becomes a fault.
void Forget();

// The HOST button, for the self-check to AIM at. Null before Build.
//
// A pointer rather than a "click it for me" helper on purpose: a test that calls DoHost()
// directly proves the function runs and nothing else, while the defect this screen has
// actually suffered is a control that draws and cannot be reached. The self-check reads
// this widget's screen rect, puts the real cursor on it and delivers a real press-release,
// so what passes is the whole path -- layout, hit test, routing -- and not just the tail.
void* HostButton();

}  // namespace ui::server_browser_actions
