// ui/server_browser_panels.h -- THE TWO PANES BESIDE THE LIST: what the chosen server IS,
// and what the SCREEN currently knows.
//
// WHY THEY EXIST AT ALL. The browser used to be a five-column table with a one-line footer:
// every fact about a server had to fit in a column ~130 px wide, and every fact about the
// screen had to fit in one sentence that the next sentence overwrote. The user rejected that
// whole shape ("это дизайн говно у сервер браузера"), and the redesign's root
// (docs/SERVER_BROWSER_ARC.md section 7.1) is VOTV's OWN save browser -- list on the left,
// a details panel top-right, a black status pane under it. This module is those two panes.
//
// WHY ONE TU FOR BOTH. They are two halves of one idea -- "everything that is not the list"
// -- they are built in the same pass, repainted on the same tick, and they share the one
// mechanism that makes repainting them affordable (below). Splitting them would duplicate
// that mechanism into two files whose only difference is which strings they render.
//
// THE MECHANISM, and it is the reason this is not a naive Sync(): EVERY LINE IS WRITTEN
// ONLY WHEN ITS TEXT CHANGES. A UTextBlock write is a ProcessEvent dispatch, and these two
// panes are ~11 lines; painting them every menu tick would be ~1,300 dispatches a second on
// a screen whose content changes about once every five seconds. So each line holds the
// string it last rendered and compares before it writes. The comparison is a std::string
// == on short text; the dispatch it avoids is an engine call.
//
// THREADING: game thread only, like every other native screen module.

#pragma once

namespace ui::server_browser_panels {

// ---- construction -------------------------------------------------------------------

// Build the DETAILS panel ("Server info:") into `parent`, a UVerticalBox. False if a widget
// could not be spawned -- the caller fails the whole build rather than shipping a hole.
bool BuildDetails(void* parent);

// Build the STATUS pane into `parent`, a UVerticalBox. Black fill: this is the one place
// VOTV's own save browser goes fully black behind text, and it is the pane it puts there.
bool BuildStatus(void* parent);

// The menu instance died and took the widgets with it.
void Forget();

// ---- painting -------------------------------------------------------------------------

// Repaint both panes from current state. Safe to call every tick: it is rate-limited
// internally to once a second (the fastest thing on either pane is a seconds counter), and
// every line writes only on a change. `force` skips the rate limit -- pass it when
// something the player just did must show immediately (a selection, a click's answer).
void Sync(bool force);

// A CLICK'S ANSWER, held against the ordinary status text for a few seconds.
//
// This used to be the browser screen's own `SetNotice` writing over the single footer line.
// The status pane has a line of its own for it, so an answer no longer erases the server
// count -- but it still expires, because a sentence about an action the player took ten
// minutes ago is not status.
void SetNotice(const char* utf8);

}  // namespace ui::server_browser_panels
