// coop/input/input_owner.h -- WHO owns the keyboard right now.
//
// THE PROBLEM THIS EXISTS FOR (GitHub issue #5, "Unable to sv.request"): our WndProc
// detour swallowed keys knowing only whether one of OUR surfaces was up. It had no term
// -- none, anywhere in the mod -- for "the GAME is taking typed text right now". So a
// player standing at VOTV's in-game console could not type `sv.request`, because the `t`
// opened our chat instead. Measured end to end in
// research/findings/tooling/votv-input-ownership-FACTS-2026-07-31.md:
//
//     KEYDOWN 0x54 ('T') -> SWALLOWED by the T-chat hotkey   [capture=0 chat=0]
//     CHAR    0x435      -> SWALLOWED by CaptureActive       [capture=1 chat=1]
//
// The character reached nobody but ImGui. VOTV has 26 such text surfaces / 73 fields, so
// an allowlist of surfaces would be a site list; this is the invariant instead.
//
// THREE INDEPENDENT TERMS, NOT ONE ENUM. "Which of our surfaces is up", "does the game
// own typed text" and "are we the foreground window" are three axes that are true and
// false independently -- TAB opens a game interface while our chat is open; the loading
// cover owns input while owning no text; alt-tab happens with anything up. Fusing them
// into one value is the defect docs/LESSONS.md records under "A readiness ANNOUNCEMENT is
// not evidence of the VISIBLE state it precedes" (second instance), where CaptureActive()
// silently counted LoadingOpen() and made a marker that was true about the SESSION and
// false about the INPUT PATH. Each consumer reads the term it means. (Cited by TITLE, not
// by line: this comment said `LESSONS.md:794-798` until 2026-07-31, which is the
// roster-SCREENSHOT lesson -- a different finding entirely. See
// `[[lesson-a-comment-citing-a-dependency-line-number-rots-silently]]`.)
//
// STALENESS IS PER TERM, AND SO IS THE FAIL DIRECTION:
//   - OverlayOwnsText / IsForeground are OUR OWN state, read synchronously in-process.
//     Zero staleness. A consumer may fail CLOSED on them.
//   - GameOwnsText is republished by a game-thread tick and read as a relaxed atomic, so
//     it can be stale. Consumers MUST fail OPEN on it: when it is unknown or stale we do
//     NOT take the key, so a stale predicate costs a hotkey rather than a character.
//
//     CORRECTED 2026-07-31 -- this block used to claim a stale predicate "can never cost
//     a character". MEASURED, that is false in BOTH directions:
//       false->true is bounded by the FAST tick (~100 ms), so a player who focuses a game
//         field and types inside that window DOES lose the character;
//       true->false is bounded by the FULL scan (~1 s), because TickGameThread's
//         `!doFullScan` branch deliberately never stores false -- so every mod hotkey is
//         dead for up to a second after a game field loses focus.
//     Both windows are removable and neither is removed yet: `gate3` measured that
//     WndProcDetour RUNS ON THE GAME THREAD (`overlay_diag::NoteWndProcThread`, logged
//     `isGameThread=1`), so the WndProc can evaluate this predicate SYNCHRONOUSLY at the
//     keydown instead of reading a polled republish. That is designed, not built --
//     research/findings/tooling/votv-input-bindings-cursor-DESIGN-2026-07-31.md §5.
//
// WHAT `GameOwnsText` IS, AND WHY IT IS NOT THE OBVIOUS THING (measured 2026-07-31):
// the per-FIELD predicate does not exist. `UWidget::HasKeyboardFocus()` on a live,
// on-screen `UEditableTextBox` reads FALSE even right after calling the engine's own
// `SetKeyboardFocus()` on it, because UMG tests the cached `SObjectWidget` wrapper rather
// than the inner `SEditableTextBox`. The same call on the OWNING USER WIDGET reads TRUE.
// So the implementable invariant is "a game UUserWidget holds keyboard focus", which
// covers all 26 surfaces and any VOTV adds later. `mainPlayer.activeInterface` is checked
// FIRST as a fast path (one pointer read, and in the common case the scan exits after a
// single UFunction call), but it is NOT the gate: a paper census found 8 of the 26 text
// surfaces with no interface-driving owner, including save-slot renaming and the settings
// search.

#pragma once

namespace coop::input::input_owner {

// ---- publishers -------------------------------------------------------------

// Game thread. `doFullScan` picks the cadence: false = the fast path only (a pointer read
// plus one UFunction call, covering everything reachable through `Enter Interface`), true =
// also walk GUObjectArray for any focused UUserWidget (the 8 census outliers). Call the
// fast form at ~10 Hz and the full form at ~1 Hz; the full walk at frame rate would be the
// per-frame full-array scan this project bans. Never called from the WndProc or the
// render thread.
void TickGameThread(bool doFullScan);

// Render thread, once per frame, from the overlay: does one of OUR text fields have
// focus (ImGui's WantTextInput, or the chat bar's own open latch).
void PublishOverlayOwnsText(bool owns);

// ---- readers (any thread, lock-free) ----------------------------------------

// A game UI holds keyboard focus: VOTV's console, notebook, laptop, inventory search,
// save-slot rename, settings search... Up to one game-thread tick stale, so callers
// MUST treat it as "do not take the key" rather than as permission.
bool GameOwnsText();

// One of OUR ImGui text fields has focus. Our own state; exact.
//
// AS-BUILT, 2026-07-31: PUBLISHED BUT NOT YET READ -- zero consumers. It exists because
// the arbiter's contract is the three terms together and the AXIS-2 (rebindable keys)
// work consumes it; until then it is write-only, and that is a smell recorded rather
// than hidden. If AXIS 2 does not land, this accessor goes (RULE 2).
bool OverlayOwnsText();

// The foreground window belongs to this process.
//
// AS-BUILT, 2026-07-31: this DUPLICATES `ui::input_focus::IsOurWindowForeground()`, which
// still has 14 live call sites across freecam / spawn_menu_unlock / voice_capture /
// voice_chat / multiplayer_menu. The collapse of those into this arbiter is DESIGNED and
// NOT DONE. Do not read the design intent as the state of the tree -- two answers to
// "who owns the keyboard" exist side by side right now, which is the RULE-2 debt this
// arc opened and owes.
bool IsForeground();

// The composite every hotkey wants: may this key be taken away from the game?
// False whenever the game owns typed text, whenever we are not foreground, or whenever
// the answer is not yet known. Fails OPEN toward the game by construction.
bool MayTakeKey();

// Diagnostics for the log line / F1 panel; never a control-flow input.
const char* LastGameOwnerName();

}  // namespace coop::input::input_owner
