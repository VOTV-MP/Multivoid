// coop/dev/native_text_probe.h -- CAN A NATIVE UMG FIELD TAKE TYPED TEXT IN OUR TREE?
// The shipped default surface gives a player no way to connect by IP: ConnectDirect is complete and
// the ImGui browser has driven it since it was written, but the address field never crossed to the
// native browser, because nothing in this tree has ever taken typed text outside ImGui. Every
// design for fixing that forks on one unmeasured fact -- does a UEditableTextBox in a hand-wired,
// never Initialize()d widget tree receive keystrokes?
//
// It asserts on the TEXT, not on a focus flag, because focus is the axis already known to lie:
// HasKeyboardFocus() reads false on a live on-screen field, UMG testing the cached SObjectWidget
// wrapper, and true on the owning user widget, which our screens do not have. So it synthesizes
// real WM_CHARs and reads the field's Text back, and it CREATES that condition rather than waiting
// for someone to type, so a silent run is a real negative rather than an untested one.
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
