// coop/dev/native_text_probe.h -- CAN A NATIVE UMG FIELD TAKE TYPED TEXT IN OUR TREE?
//
// THE ONE QUESTION, and the whole reason this file exists as its own instrument:
//
//     A player on the shipped default surface has no way to connect by IP. The
//     feature is not broken -- `session_manager::ConnectDirect` is complete and the
//     ImGui browser has driven it since it was written -- but the NATIVE browser
//     became the permanent default (2026-08-30) and the address field did not come
//     across, because nothing in this tree has ever taken typed text outside ImGui.
//
// Every design for fixing that forks on ONE unmeasured fact: does a `UEditableTextBox`
// placed in a HAND-WIRED, never-`Initialize()`d widget tree receive keystrokes? The
// standing note says a per-FIELD Slate focus predicate does not exist -- `HasKeyboardFocus()`
// reads false on a live on-screen field because UMG tests the cached `SObjectWidget`
// wrapper, and true on the owning USER WIDGET
// (`research/findings/tooling/votv-input-ownership-FACTS-2026-07-31.md`). Our screens
// have no user widget in the normal sense. So the honest answer today is `[?]`, and
// three different implementations follow from three different answers.
//
// WHY IT ASSERTS ON THE TEXT AND NOT ON THE FOCUS FLAG. A focus boolean is the axis the
// prior finding says is BROKEN, so a probe that reads it measures the thing already known
// to lie. This one synthesizes real `WM_CHAR`s and then reads the field's own `Text`
// property back: the artifact, not the announcement. If the characters arrive, the
// mechanism works whatever any focus predicate claims; if they do not, no focus reading
// would have saved it. (`[[lesson-an-instrument-blind-to-the-failing-axis-grades-itself-green]]`,
// and the reason this session's hit-test hunt only ended when the probe printed the
// artifact -- the child table -- instead of a derived answer.)
//
// IT CREATES THE CONDITION ITSELF. An instrument that waits for a human to type proves
// nothing when nobody types; this one posts the characters, so a silent run is a real
// negative rather than an untested one
// (`[[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]`).
//
// SEPARATE TU FROM `native_ui_probe` ON PURPOSE. That file is 1,000 LOC -- already 200
// past the soft cap -- and it answers a different question (class/function resolution,
// donor residency, the switcher child map, whether the ImGui substrate is retirable).
// This one is about INPUT. Folder-per-domain-concept and the modular rule both say a new
// concept gets its own file rather than a fourth rung on a file that is over budget.
//
// DEV-ONLY, and it WRITES: it adds a field to the live native browser panel, focuses it,
// posts characters into the game window, and removes it again. Gated behind its own
// config row so it can never run for a player.

#pragma once

namespace coop::dev::native_text_probe {

// Called once per native-browser tick, with the browser's own content panel. Does
// nothing unless `native_text_probe=1`. Runs its whole sequence ONCE per process and
// then latches: the verdict is a property of the build, not of the frame.
//
// `panel` may be null (the screen is not built yet) -- the probe simply waits.
void Tick(void* panel);

}  // namespace coop::dev::native_text_probe
