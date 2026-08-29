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
//     BOTH STALENESS WINDOWS ARE NOW CLOSED for the dominant term (2026-07-31). They
//     were real: false->true bounded by the FAST tick (~100 ms) cost a CHARACTER, and
//     true->false bounded by the FULL scan (~1 s) left every hotkey dead for up to a
//     second, because TickGameThread's `!doFullScan` branch deliberately never stores
//     false. `MayTakeKey` now evaluates the interface term SYNCHRONOUSLY when it is
//     called on the game thread -- which every WndProc hotkey edge is (`gate3`:
//     `overlay_diag::NoteWndProcThread` logged `isGameThread=1`). Only the 1 Hz
//     GUObjectArray scan for the interface-less menu surfaces is still polled, and its
//     staleness can cost a hotkey, never a character.
//
// WHAT `GameOwnsText` IS -- TWO TERMS, BECAUSE VOTV HAS TWO WAYS TO DELIVER A CHARACTER.
// This is the whole finding, and it is why the first shipped attempt (f03c04f0) did not
// close issue #5. Corrected 2026-07-31 after the user ran GATE 0 at the SAT console and
// V/T still fired.
//
//   1. `mainPlayer.activeInterface` is valid -- THE DOMINANT TERM, and it is the GAME'S
//      OWN GUARD, not our approximation of one. Measured in mainPlayer's ubergraph: the
//      block that handles every key does `IsValid(activeInterface)` and, if so,
//      `WidgetInteraction.SetFocus(activeInterface)` then `WidgetInteraction.PressKey`.
//      So the game consumes a typed key IFF that field is valid -- and it delivers it
//      through a VIRTUAL USER, because the in-world screens (SAT console, laptop, arcade,
//      TV, radar, portable PC) are UMG inside a `UWidgetComponent` driven by mainPlayer's
//      `UWidgetInteractionComponent`.
//
//   2. A game `UUserWidget` holds USER-0 Slate focus, itself or via a descendant -- the
//      1 Hz backstop for the 8 of 26 census surfaces with no interface-driving owner
//      (save-slot rename, settings search).
//
// WHY THE OBVIOUS PREDICATE CANNOT WORK, and why it looked like it did:
// `UWidget::HasKeyboardFocus()` asks about USER 0. It is structurally blind to term 1's
// virtual user, so it answers FALSE at every in-world screen -- the reporter's exact
// surface. It answered TRUE for the player inventory only because `setActiveInterface`
// ALSO calls `SetInputMode_GameAndUIEx`, which focuses the same widget for user 0. One
// measurement, one surface, two delivery paths, and only one of them modelled.
// (`HasKeyboardFocus` is additionally an EXACT-widget test, so even on the user-0 path it
// goes false the moment focus moves into a field -- hence the `HasFocusedDescendants` OR
// in term 2. That was the *previous* theory of this bug; it is real, but it is not what
// issue #5 was.)

#pragma once

namespace coop::input::input_owner {

// ---- publishers -------------------------------------------------------------

// Game thread. `doFullScan` picks the cadence: false = the fast path only (a pointer read
// plus one UFunction call, covering everything reachable through `Enter Interface`), true =
// also walk GUObjectArray for any focused UUserWidget (the 8 census outliers). Call the
// fast form at ~10 Hz and the full form at ~1 Hz; the full walk at frame rate would be the
// per-frame full-array scan this project bans. Never called from the WndProc or the
// render thread.
//
// WHAT THE FULL SCAN COSTS, MEASURED 2026-08-29 -- read this before adding a term to it.
// The full pass is 88% of every blueprint call this mod makes: `HasKeyboardFocus` 44.2% +
// `HasUserFocusedDescendants` 44.2% of 103,057 sampled coop dispatches, i.e. ~6,500 reflected
// UFunction dispatches PER SECOND -- and because it is ONE posted task they all land in a
// SINGLE frame, against a normal frame's ~858 game-thread dispatches. That is the p90 of
// 8.9 ms/frame against a 0.6 ms median, and the [HITCH] lines with it.
//
// It was worse: the scan filtered only the `Default__` prefix, so it also interrogated every
// widget TEMPLATE inside a WidgetBlueprintGeneratedClass -- objects that can never hold Slate
// focus. `IsLiveWidgetInstance` now rejects them (~9,300/s -> ~6,500/s). Steady fps did NOT
// move: this is a 1 Hz spike, not a per-frame tax, so it is a STUTTER defect and was never a
// candidate for the separate 120 -> 70 fps regression.
//
// The proper fix is not a cheaper filter: it is to stop POLLING. The question only matters
// when one of our four hotkeys arrives, and `gate3` measured that WndProcDetour runs on the
// GAME THREAD, so it can be answered synchronously there (term 1 already is). Not done --
// a naive move makes a hotkey press pay the whole scan, so it needs the scan narrowed first.
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

// The composite every hotkey wants: may THIS key be taken away from the game?
// False whenever the game owns typed text, whenever we are not foreground, or whenever
// the answer is not yet known. Fails OPEN toward the game by construction.
//
// `vk` is the Windows virtual-key code, and it is a REQUIRED argument rather than a
// convenience: "the game owns typed text" is not a property of the moment alone, it is
// a property of the moment AND the key. With an interface open the game forwards
// EVERY key into the focused widget -- but a widget does something with a letter and
// nothing whatsoever with F1. Answering per-moment would have made every mod hotkey
// dead inside every game interface, which is a regression nobody asked for; answering
// per-key keeps F-keys usable at the SAT console while `T`, `V` and tilde correctly
// go to the game. It is also what makes the rebind escape hatch actually work: a
// player who moves chat to F2 can then type at the console AND chat.
bool MayTakeKey(unsigned vk);

// Diagnostics for the log line / F1 panel; never a control-flow input.
const char* LastGameOwnerName();

}  // namespace coop::input::input_owner
