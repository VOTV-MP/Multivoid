// ue_wrap/devices/oven_screen.h -- engine access for the kitchen oven's repair screen (UI_oven_C),
// the widget each peer's kitchen_C creates at BeginPlay and drops in fix(). Principle-7
// engine-wrapper layer: no network logic; coop::oven_progress_sync drives the mirror through here.
//
// The screen keeps the repair's first steps as five bools nothing sets back: one per screw a
// screwdriver took out (TL_unscrewed, TR_unscrewed, BL_unscrewed, BR_unscrewed) and lid_taken, which
// the lid's press sets once all four are out. Its upd() shows or hides the lid from lid_taken. The
// wiring under the lid is drawn at random on each screen, so it is each peer's own. The save
// carries none of this: a loaded oven starts with every screw in.

#pragma once

#include <cstdint>

namespace ue_wrap::oven_screen {

inline constexpr uint8_t kScrewTL = 1u << 0;
inline constexpr uint8_t kScrewTR = 1u << 1;
inline constexpr uint8_t kScrewBL = 1u << 2;
inline constexpr uint8_t kScrewBR = 1u << 3;
inline constexpr uint8_t kLid     = 1u << 4;
inline constexpr uint8_t kAll     = 0x1F;

// The oven a repair screen belongs to (its `kitchen`), or null when it names none. Game thread.
void* OvenOfScreen(void* screen);

// The oven's repair screen when a player has it open (enter() made it visible, quit() has not collapsed it), else
// null: an oven with no screen, a collapsed one, or a visibility that does not read. Read BEFORE the oven's fix(),
// which drops the screen from the oven. Game thread.
void* OpenScreenOf(void* oven);

// Run the screen's own quit(): the game's exit of it (the input mode back to the game, the cursor off, the player's
// active interface cleared, the screen collapsed). False when it did not run. Game thread.
bool CallScreenQuit(void* screen);

// The five bools of `oven`'s screen as a mask. False when the oven has no screen (it is repaired, or
// not begun play) or a member does not resolve. Game thread.
bool TryReadProgress(void* oven, uint8_t& mask);

// Set the bools `mask` names on `oven`'s screen and repaint it (upd). Never clears one. False as
// TryReadProgress. Game thread.
bool RaiseProgress(void* oven, uint8_t mask);

}  // namespace ue_wrap::oven_screen
