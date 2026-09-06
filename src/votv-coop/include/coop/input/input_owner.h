// coop/input/input_owner.h -- who owns the keyboard right now. The window-procedure detour
// swallows keys, so it must know whether the game is taking typed text: at the game's console
// the chat hotkey opened our chat instead of typing the letter (issue 5), and with dozens of
// game text surfaces an allowlist would be a site list; this is the invariant instead. Three
// independent terms, not one enum: which of our surfaces is up, whether the game owns typed
// text, and whether we are the foreground window vary independently, and each consumer reads
// the term it means. Staleness and the fail direction are per term: the overlay and foreground
// terms are our own state, read synchronously, so a consumer may fail closed on them; the game
// term is republished by a game-thread tick and read as a relaxed atomic, so consumers fail
// open on it, a stale answer costing a hotkey rather than a character. The game term has two
// halves: the player's active interface is valid (the game's own guard: its key handler
// focuses that interface and presses the key through a virtual user, which is how the in-world
// screens receive text), evaluated at each hotkey edge; and a game widget holds user-zero
// focus, the 1 Hz backstop. The widget's has-keyboard-focus alone is blind to the virtual
// user, so it answers false at every in-world screen.

#pragma once

namespace coop::input::input_owner {

// Publishers.

// Game thread. `doFullScan` picks the cadence: false is the fast path only (a pointer read and
// one UFunction call, covering everything reachable through the active interface), true also
// walks the object array for any focused game widget, the census outliers. The fast form at
// about 10 Hz, the full form at about 1 Hz; the full walk at frame rate would be the
// per-frame full-array scan this project bans. Never from the window procedure or the render
// thread. The full scan is most of the blueprint calls this mod makes, and as one posted task
// they land in a single frame, a stutter rather than a per-frame tax; the widget templates
// inside blueprint classes are rejected before the focus queries, since they can never hold
// focus. The proper fix is to stop polling and answer at the hotkey edge, which runs on the
// game thread; not done, since a naive move makes a hotkey press pay the whole scan.
void TickGameThread(bool doFullScan);

// Render thread, once per frame, from the overlay: does one of our text fields have focus
// (the ImGui text-input want, or the chat bar's own open latch).
void PublishOverlayOwnsText(bool owns);

// Readers, any thread, lock-free.

// A game UI holds keyboard focus: the console, the notebook, the laptop, the inventory search,
// the save-slot rename, the settings search. Up to one game-thread tick stale, so a caller
// treats it as do-not-take-the-key rather than as permission.
bool GameOwnsText();

// One of our ImGui text fields has focus. Our own state; exact.
bool OverlayOwnsText();

// The foreground window belongs to this process. The same answer as the input-focus module's
// foreground check, which still has its own call sites; two owners of one question, a
// collapse not yet done.
bool IsForeground();

// The composite every hotkey wants: may this key be taken away from the game? False whenever
// the game owns typed text, whenever we are not foreground, or whenever the answer is not yet
// known; fails open toward the game by construction. `vk` is required rather than a
// convenience: the game owning typed text is a property of the moment and the key, since with
// an interface open the game forwards every key into the focused widget, which does something
// with a letter and nothing with a function key. Per moment, every mod hotkey would die inside
// every game interface; per key, the function keys stay usable at the console while the chat,
// voice and player-list keys go to the game, and a player who rebinds chat to a function key
// can type at the console and chat.
bool MayTakeKey(unsigned vk);

}  // namespace coop::input::input_owner
