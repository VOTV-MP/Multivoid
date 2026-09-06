// ui/server_browser_rows.h -- THE SERVER LIST ITSELF: the row widgets, the data behind them,
// the identity pairing between the two, and the hover and selection that read it. The window
// around it -- scrim, frame, title strip, footer, ESC and the chrome polls -- stays in
// ui/server_browser_native.cpp, which drives this one. The API says WHAT the screen wants and
// never HOW a row is realised: nothing below names a child index, a widget count or a
// UScrollBox. The column table is private because its two consumers, the header strip and
// every row, must agree on the fill weights or the columns do not line up. Game thread only:
// every function here spawns UObjects or calls UFunctions.
//
// THE INVARIANT THIS MODULE EXISTS TO HOLD: a row's identity is its `lobbyId`, never its
// index. The master emits lobbies in arbitrary order and the client imposes a total order at
// the parse site (`lobby_client.cpp`), which makes it stable for a GIVEN SET and nothing more.
// One host leaving while another joins gives the same count with different members and shifts
// every row after it, so an index cannot identify a row across a refresh. A row remembers the
// id it was RENDERED with, captured with its text by the single writer (`Sync`).

#pragma once

#include "coop/net/lobby_client.h"   // LobbyRow -- what Selected() hands back

#include <cstdint>

namespace ui::server_browser_rows {

// ---- construction -------------------------------------------------------------------

// Adopt the panel the rows live in, and forget every row identity. Call it with the new
// panel when the screen is built, and with `nullptr` when the menu instance dies and the
// widgets die with it -- both sites want exactly this pair of effects.
void Attach(void* listPanel);

// The panel, for the callers that must pass it on (the self-check drives it directly).
void* Panel();

// THE SCREEN IS BEING PRESENTED: forget where the pointer was.
//
// The hover answer is re-evaluated only when something could have CHANGED it -- the pointer
// moved, the list scrolled, the row count changed. None of those happen when a screen is
// closed and reopened with a still hand, so without this the list comes back highlighting
// whatever row was under the cursor last time, over a list that has since been refetched --
// and that index is what a click reads.
void OnShown();

// ---- the list -----------------------------------------------------------------------

// THE SINGLE WRITER of the rows' text, their tints and their id pairing. Pulls the current
// network list, grows the panel if it is short, collapses the surplus, and repaints.
void Sync();

// The fetch generation `Sync` last PAINTED. The screen compares it against
// `session_manager::RowsGeneration()` so a lobby that arrives between two timed fetches is
// drawn on arrival rather than at the next tick of the 5 s cadence.
uint64_t PaintedGeneration();

// HOW MANY SERVERS THE MASTER LISTED, and HOW LONG AGO WE HEARD IT (ms; 0 = never).
//
// The DATA count, not the number of row widgets: the two differ whenever the list is longer
// than the pool, and the status pane's job is to report the world, not our rendering of it.
int      Count();
uint64_t MsSinceFetch();

// A ROW'S AGE AS OF NOW -- the master's seconds-since-heartbeat plus however long we have
// been holding the list. `ageSec` alone is a snapshot that stops being true on arrival, and
// two readers already needed the corrected value (the row dim and the details panel), which
// is why it is here rather than open-coded at each.
int AgeNowSec(const coop::net::lobby::LobbyRow& r);

// ---- pointer ------------------------------------------------------------------------

// Re-evaluate which row the pointer is on, and edge-apply the text recolour. Cheap on a
// tick where nothing could have changed it (one dispatch, no walk).
void UpdateHover();

// A left-button RELEASE landed on the list: select the hovered row, if there is one.
// Returns true if the selection changed. The screen calls this after its own chrome has
// had the first refusal, so a click on a footer button is never also a row click.
bool ClickSelect();

// ---- what is hovered / chosen -------------------------------------------------------
//
// Both facts have no observer outside the pixels -- a highlight that never appears looks
// exactly like a cursor that was never there -- so the self-check asserts on them.
int         HoveredRow();
const char* SelectedId();

// The chosen row's DATA, false when nothing is chosen. Resolved BY LOBBY ID against the
// rows last rendered (invariant above). A selection whose lobby has since vanished from
// the list answers false and drops the highlight, because the host quit while the screen
// was open and there is nothing left to connect to.
bool Selected(coop::net::lobby::LobbyRow& out);

// ---- diagnostics --------------------------------------------------------------------

// WHY ROW `i` DID NOT HOVER: its parts with their live visibility, rects and `IsHovered`. It
// reports a signal production no longer consumes -- the walk moved to geometry -- and that is
// the point: the question is whether Slate agrees with the rects, and the answer being NO
// inside a UScrollBox is the measurement this row model rests on.
void LogRowHitDiagnostics(int32_t i);

}  // namespace ui::server_browser_rows
