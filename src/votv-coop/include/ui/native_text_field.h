// ui/native_text_field.h -- a text field for the native screens, which owns its own input.
// Not an editable text box, and that is measured rather than assumed: one spawned into the
// live browser panel, attached, made visible, with its hint and text set programmatically
// and keyboard focus granted (all succeeded), then fed two real character messages, read its
// own text back as empty. Slate does not route keystrokes into a hand-wired, never
// initialised widget tree, and our native screens are exactly that. So the engine gives us
// pixels and geometry but not keyboard delivery, and the field owns the input half itself:
// our window-procedure detour already sees the character messages and, when no overlay
// surface holds capture, passes them to the game; a native field claims them at that seam
// and renders the result into an ordinary text block, which the same probe proved we can
// drive. One primitive, not one field: two shipped screens need it (the browser's address
// box, the hosting window's name). Deliberately not a general text editor: no selection, no
// mouse caret placement, no IME; an address and name box. Clipboard paste is built (Ctrl+V,
// entry-trimmed, control characters dropped, appended, capped). Case and style follow the
// game's own UI: its border material over its grey, its menu font, and no all-caps labels.

#pragma once

#include <cstdint>
#include <string>

namespace ui::native_text_field {

// An opaque handle. Owned by the screen that created it; destroyed with that screen.
struct Field;

// Build a bordered field into `parent` and return it. `hint` is the placeholder drawn while
// the field is empty and unfocused (MTA's shape: a label parented to its own edit box).
// `maxLen` bounds the stored string in code points (a held key would otherwise overflow
// it). Null if the widget kit fails to build.
Field* Create(void* parent, const wchar_t* hint, int32_t maxLen, float widthPx);

// Everything Destroy does except touching the engine: unhook the focus, drop the registry
// row, free the handle. For a tree that died with its menu instance. The distinction is not
// theoretical: a screen once called Destroy on the menu-instance death edge and dispatched
// the child removal into a freed tree, and the reflection call has no liveness check, so a
// reused object-array slot makes that a call against an unrelated object rather than a
// clean fault. Both sibling screens already drop their pointers and touch nothing on that
// edge; this lets a field do the same.
void Release(Field* f);

// Our focus, not Slate's: the engine's keyboard-focus predicate reads false on a field it
// has just successfully focused. Exactly one field in the process holds focus.
void Focus(Field* f);
void Blur(Field* f);
bool Focused(const Field* f);

// The content. `Text` is UTF-8 (the wire and the ini both take UTF-8; the widget layer
// converts once, on write).
const std::string& Text(const Field* f);
void SetText(Field* f, const std::string& utf8);

// Per tick: repaints when the string or the caret phase changed, and takes focus when the
// pointer is pressed inside the field's own rect (by geometry, the one hit-test mechanism
// these screens have).
void Tick(Field* f);

// True once after Escape was pressed while this field held focus, meaning the field ate that
// Escape to leave itself, and the owning screen must not also act on it. Asking whether any
// field is focused could never work: the blur happens on the key down and the screens take
// their edge on the key up, so the field had always let go by the time they asked, and one
// press both blurred the field and closed the window, discarding whatever had been typed.
// This latch is set by the same event that consumes the key, so there is no ordering to get
// wrong.
bool ConsumeEscape(Field* f);

// True once after Enter was pressed while this field held focus. The caller connects, saves,
// or whatever Enter means to it; MTA wires the same edge to its connect handler.
bool ConsumeSubmit(Field* f);

// The window-procedure seam. Called from the input detour before the message reaches the
// game; return true to swallow. Both are no-ops returning false when no field holds focus,
// the overwhelmingly common case, at the cost of one atomic read. The swallow is the point
// and also the hazard: while a field has focus we take keys the game would otherwise act on,
// correct for a menu-time browser, and why focus is dropped on Escape, on Blur, and whenever
// the owning screen closes.
bool OnChar(wchar_t c);
bool OnKeyDown(int vk);

// Is any native field holding the keyboard right now? The detour asks this before doing its
// own hotkey work, and the input owner asks it so the game is not told we are idle while we
// are eating its keystrokes.
bool AnyFocused();

// The selftest of the editing rules (append, the cap, backspace including surrogate pairs,
// the Enter edge, Escape to blur). Pure logic, no widget, no game, so it runs at boot on
// every build rather than behind a flag nobody remembers to arm. False and a log per
// failure. Does not cover the message delivery, a live-process question.
bool RunSelftest();

}  // namespace ui::native_text_field
