// ui/server_browser_actions.h -- the native server browser's action bar: CONNECT, HOST and
// REFRESH. Before it, the native screen could show servers and select one and then do nothing with
// either, and `ui/host_window_native` had no way in but a dev flag.
//
// WHY A SEPARATE TU. `server_browser_native.cpp` is past the 800-LOC soft cap, and the modular
// rule's extraction trigger says new code that is conceptually its own subsystem goes in its own
// file rather than growing that one further. An action bar qualifies: it owns three buttons, their
// placement, their click routing and the sentences they produce, and holds no row state and no
// screen state. (The row model is a second, larger extraction still owed.)
//
// PRINCIPLE 7: this is presentation. It authors no hosting and no joining of its own -- it calls
// the same `coop::session_manager` entry points the ImGui browser calls, so the two surfaces
// cannot drift into two different meanings of "Connect". Game thread only, like every other
// native-screen TU.

#pragma once

namespace ui::server_browser_actions {

// Build the ACTION GRID into `parent` (a UVerticalBox), styled from `donorBtn` (the game's own
// `ui_saveSlots_C.button_back`, so the buttons carry its press and hover sounds).
//
// A GRID UNDER THE LIST, NOT A ROW IN THE FOOTER, mirroring VOTV's own save browser: a block of
// large framed action buttons directly beneath the list they act on, with the footer left to
// `Back` alone. The cell table lives in the .cpp and the grid renders however many cells there
// are, so a variant can add one without reshaping anything here.
//
// Returns false if any button could not be built; the caller treats that as a build failure rather
// than shipping a grid with a hole in it.
bool Build(void* parent, void* donorBtn);

// Build CONNECT on its own, into the RIGHT-HAND column directly under the details panel.
//
// It is not in the grid because it is not the same KIND of action: every cell in the grid is true
// whatever is selected, and this one acts on the selection the panel above it is describing.
// Putting the verb under its subject is also what makes the panel worth reading before pressing
// it.
bool BuildConnect(void* parent, void* donorBtn);

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

// The three buttons, for the self-check to AIM at. Null before Build.
//
// Pointers rather than "click it for me" helpers on purpose: a test that calls DoConnect()
// directly proves the function runs and nothing else, while the defect this screen has
// actually suffered is a control that draws and cannot be reached. The self-check reads
// each widget's screen rect, puts the real cursor on it and delivers a real press-release,
// so what passes is the whole path -- layout, hit test, routing -- and not just the tail.
void* HostButton();
void* ConnectButton();
void* RefreshButton();

// WHAT THE LAST CLICK DECIDED, as a short stable token. Empty until one is handled.
//
// It exists because every one of CONNECT's outcomes is a SENTENCE IN THE FOOTER, and a sentence is
// not observable to anything but a human reading the screen -- so "the button is wired" and "the
// button did nothing" produce identical evidence.
//
// Tokens: "connect:none" (nothing selected) / "connect:self" (your own server) /
// "connect:started" / "connect:busy" / "host" / "refresh". They are TOKENS, not the player-facing
// text: the sentences are free to be reworded and translated, and an assertion must not break when
// they are.
const char* LastOutcome();

}  // namespace ui::server_browser_actions
