// coop/dev/input_focus_probe.h -- see the .cpp for what each sample measures.
//
// DIAGNOSTIC ONLY, armed by VOTVCOOP_INPUT_PROBE=1. Decides whether a reflected
// UWidget::HasKeyboardFocus() over the three engine editable-widget classes is a
// usable "the game owns typed text" predicate (the input-ownership arc), and whether
// the camera spins while our overlay suppresses VOTV's cursor recentre.

#pragma once

namespace coop::dev::input_focus_probe {

// True when VOTVCOOP_INPUT_PROBE=1. Cheap after the first call.
bool IsArmed();

// Call once per presented frame. No-ops unless armed; at most one game-thread
// sample per second (the scan walks GUObjectArray, so it is never a hot path).
void NoteFrame();

}  // namespace coop::dev::input_focus_probe
