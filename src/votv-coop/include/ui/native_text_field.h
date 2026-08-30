// ui/native_text_field.h -- A TEXT FIELD FOR THE NATIVE SCREENS, WHICH OWNS ITS OWN INPUT.
//
// WHY THIS IS NOT A `UEditableTextBox`. Because that does not work here, and it is
// measured rather than assumed: `[V]` 2026-08-30, `coop/dev/native_text_probe` spawned
// one into the live browser panel, attached it, made it Visible, set its hint and its
// text programmatically (both succeeded), called `SetKeyboardFocus` (succeeded), posted
// two real `WM_CHAR`s to the game window, and read the field's own `Text` property back:
// empty. Slate does not route keystrokes into a hand-wired, never-`Initialize()`d widget
// tree, and our native screens are exactly that -- which is also why they render at all.
//
// So the engine will give us pixels and geometry but not keyboard delivery. The field
// therefore owns the input half itself: our `WndProcDetour` already sees `WM_CHAR`
// (`imgui_overlay.cpp:319,327`) and, when no ImGui surface holds capture, currently
// passes it to the GAME. A native field claims it at that seam and renders the result
// into an ordinary `UTextBlock`, which the same probe proved we can drive.
//
// ONE PRIMITIVE, NOT ONE FIELD. Two shipped screens need this, which is the rule of three
// at two and counting: the browser has no direct-IP address box (the user's report,
// 2026-08-30: "нету возможности нигде по айпи подключиться - НИГДЕ"), and the hosting
// window cannot name a world -- its own header records that as deferred "until a
// focusable native field is measured". It has now been measured, and the answer was no,
// so the field is built rather than waited for.
//
// WHAT IT DELIBERATELY IS NOT: a general text editor. No selection, no clipboard, no
// mouse caret placement, no IME. It is an address/name box. Those are real omissions and
// they are listed here rather than discovered later; a player who needs to paste will
// notice, and that is the next increment, not a hidden defect.
//
// CASE AND STYLE come from `docs/VOTV_UI_STYLE.md`: the frame is the game's `#646464`
// border over `#313131`, the text is `font_ui` at the donor's own size, and labels are
// never all-caps (rule 4, measured 2026-08-30).

#pragma once

#include <cstdint>
#include <string>

namespace ui::native_text_field {

// An opaque handle. Owned by the screen that created it; destroyed with that screen.
struct Field;

// Build a bordered field into `parent` and return it. `hint` is the placeholder drawn
// while the field is empty and unfocused (MTA's shape -- `CServerBrowser.cpp:492` parents
// an "Enter an address [IP:Port]" label to its own edit box). `maxLen` bounds the stored
// string in CODEPOINTS; a paste-less field cannot overflow it by accident, but a held key
// can. Null if the widget kit fails to build.
Field* Create(void* parent, const wchar_t* hint, int32_t maxLen, float widthPx);

// Tear down: removes the widgets from the parent and releases the handle. Safe on null.
// A destroyed field is removed from the focus registry first, so a WM_CHAR arriving in
// the same tick cannot reach freed memory.
void Destroy(Field* f);

// OUR focus, not Slate's -- `HasKeyboardFocus()` is the predicate the input-ownership
// finding measured as lying about a live field, and the probe above read it as 0 on a
// field it had just successfully focused. Exactly one field in the process holds focus.
void Focus(Field* f);
void Blur(Field* f);
bool Focused(const Field* f);

// The content. `Text` is UTF-8 (the wire and the ini both take UTF-8; the widget layer
// converts once, on write).
const std::string& Text(const Field* f);
void SetText(Field* f, const std::string& utf8);

// Per-tick: repaints when the string or the caret phase changed, and takes focus when the
// pointer is pressed inside the field's own rect (by GEOMETRY, via
// `native_screen::CursorOverWidget` -- the one hit-test mechanism these screens have).
void Tick(Field* f);

// True ONCE after Enter was pressed while this field held focus. The caller connects,
// saves, or whatever Enter means to it. MTA wires the same edge:
// `CServerBrowser.cpp:489` `SetTextAcceptedHandler(OnConnectClick)`.
bool ConsumeSubmit(Field* f);

// ---- the WndProc seam --------------------------------------------------------------
//
// Called from the input detour BEFORE the message reaches the game. Return true to
// swallow. Both are no-ops returning false when no field holds focus, which is the
// overwhelmingly common case and costs one atomic read.
//
// THE SWALLOW IS THE POINT AND ALSO THE HAZARD: while a field has focus we are taking
// keys the game would otherwise act on. That is correct for a menu-time browser, and it
// is why focus is dropped on Escape, on Blur, and whenever the owning screen closes.
bool OnChar(wchar_t c);
bool OnKeyDown(int vk);

// Is ANY native field holding the keyboard right now? The detour asks this before doing
// its own hotkey work, and `coop::input` asks it so the game is not told we are idle
// while we are eating its keystrokes.
bool AnyFocused();

// UN-GATED selftest of the editing rules (append, cap, backspace incl. surrogate pairs,
// the Enter edge, Escape-to-blur). Pure logic -- no widget, no game -- so it runs at boot
// on every build rather than behind a flag nobody remembers to arm. Returns false and logs
// each failure. Does NOT cover the WndProc delivery, which is a live-process question.
bool RunSelftest();

}  // namespace ui::native_text_field
