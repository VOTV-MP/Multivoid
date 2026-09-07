// coop/dev/native_text_probe.h -- CAN A NATIVE UMG FIELD TAKE TYPED TEXT IN OUR TREE?
//
// The answer is NO, and this is the instrument that measured it: a UEditableTextBox in a
// hand-wired, never Initialize()d widget tree is built, shown and fed real WM_CHARs, and reads its
// own Text back EMPTY. Slate does not route keystrokes into a tree it never initialised. What ships
// instead is ui/native_text_field, which owns its input at the window-procedure seam, and on top of
// it ui/browser_input_screens: the native direct-connect address and password boxes, the
// change-name screen and the lobby password prompt.
//
// It asserts on the TEXT and not on a focus flag, because focus is the axis already known to lie:
// HasKeyboardFocus() reads false on a live on-screen field, UMG testing the cached SObjectWidget
// wrapper, and true on the owning user widget, which our screens do not have.
//
// DEV-ONLY, and it WRITES: it adds a field to the live native browser panel, focuses it, posts
// characters into the game window and removes it again, behind its own config row.

#pragma once

namespace coop::dev::native_text_probe {

// Called once per native-browser tick, with the browser's own content panel. Does
// nothing unless `native_text_probe=1`. Runs its whole sequence ONCE per process and
// then latches: the verdict is a property of the build, not of the frame.
//
// `panel` may be null (the screen is not built yet) -- the probe simply waits.
void Tick(void* panel);

}  // namespace coop::dev::native_text_probe
