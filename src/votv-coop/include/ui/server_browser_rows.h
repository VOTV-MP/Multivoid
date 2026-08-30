// ui/server_browser_rows.h -- THE SERVER LIST ITSELF: the row widgets, the data behind
// them, the identity pairing between the two, and the hover and selection that read it.
//
// WHAT IS HERE vs WHAT IS NOT. This module owns everything that answers "what is in the
// list and which entry is the pointer on". The WINDOW that contains it -- the scrim, the
// frame, the title strip, the footer, the switcher lifecycle, ESC and the chrome click
// polls -- stays in `ui/server_browser_native.cpp`, which drives this one.
//
// WHY IT IS ITS OWN TU, and the reason is not only the line count. `docs/MULTIPLAYER_UI.md`
// section 8c.-1 step T6 is a decision about exactly this object and nothing else: keep one
// widget per row, pool a viewport's worth (~9 widgets instead of 64), or spike `UListView`.
// While the row model lived inside the screen, that decision was surgery through a
// 830-line file; behind this API it is a swap of one implementation. The API is therefore
// written to say WHAT the screen wants, never HOW a row is realised: nothing below mentions
// a child index, a widget count, or a `UScrollBox`.
//
// THE COLUMN TABLE IS PRIVATE ON PURPOSE. It has two consumers -- the header strip and
// every row -- and they must agree on the fill weights or the columns do not line up. So
// this module builds BOTH (`BuildHeader` below) and the table stays out of the header.
//
// THE INVARIANT THIS MODULE EXISTS TO HOLD (the browser header's invariant 1, moved here
// with the code that keeps it): A ROW'S IDENTITY IS ITS `lobbyId`, NEVER ITS INDEX. The
// master stores lobbies in a `HashMap` and emits `state.lobbies.values()`
// (master.rs:531-553); the client does not sort. So one host leaving while another joins
// gives the SAME COUNT in a DIFFERENT ORDER. A row remembers the id it was RENDERED with,
// captured in the same pass as its text by the single writer (`Sync`), and every click
// resolves against THAT -- never against a fresher copy of the network list.
//
// THREADING: game thread only. Every function here spawns UObjects or calls UFunctions.

#pragma once

#include "coop/net/lobby_client.h"   // LobbyRow -- what Selected() hands back

#include <cstdint>

namespace ui::server_browser_rows {

// ---- construction -------------------------------------------------------------------

// The column header strip, added to `parent` (a UVerticalBox). Built here because the
// weights it uses are the row weights.
void BuildHeader(void* parent);

// Adopt the panel the rows live in, and forget every row identity. Call it with the new
// panel when the screen is built, and with `nullptr` when the menu instance dies and the
// widgets die with it -- both sites want exactly this pair of effects.
void Attach(void* listPanel);

// The panel, for the callers that must pass it on (the self-check drives it directly).
void* Panel();

// ---- the list -----------------------------------------------------------------------

// THE SINGLE WRITER of the rows' text, their tints and their id pairing. Pulls the current
// network list, grows the panel if it is short, collapses the surplus, and repaints.
void Sync();

// The fetch generation `Sync` last PAINTED. The screen compares it against
// `session_manager::RowsGeneration()` so a lobby that arrives between two timed fetches is
// drawn on arrival rather than at the next tick of the 5 s cadence.
uint64_t PaintedGeneration();

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
// These exist because both facts had no observer outside the pixels. Hover was silently
// dead for as long as the screen existed -- a highlight that never appears looks exactly
// like a cursor that was never there -- and the self-check now asserts on them.
int         HoveredRow();
const char* SelectedId();

// The chosen row's DATA, false when nothing is chosen. Resolved BY LOBBY ID against the
// rows last rendered (invariant above). A selection whose lobby has since vanished from
// the list answers false and drops the highlight, because the host quit while the screen
// was open and there is nothing left to connect to.
bool Selected(coop::net::lobby::LobbyRow& out);

// ---- diagnostics --------------------------------------------------------------------

// WHY ROW `i` DID NOT HOVER: its parts with their live visibility, rects and `IsHovered`.
// It reports a signal PRODUCTION NO LONGER CONSUMES -- the walk moved to geometry -- and
// that is the point: the question is "does Slate agree with the rects", and the answer
// being NO inside a `UScrollBox` is the measurement this whole row model rests on.
void LogRowHitDiagnostics(int32_t i);

}  // namespace ui::server_browser_rows
