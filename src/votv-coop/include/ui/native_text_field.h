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
// WHAT IT DELIBERATELY IS NOT: a general text editor. No selection, no mouse caret
// placement, no IME. It is an address/name box. Those are real omissions and they are
// listed here rather than discovered later.
//
// CLIPBOARD PASTE WAS ON THAT LIST AND IS NOW BUILT (2026-08-31): Ctrl+V, entry-trimmed,
// control characters dropped, appended, capped. The sentence that used to stand here --
// "a player who needs to paste will notice, and that is the next increment" -- was left
// beside the code that closed it, which is the one failure an honest omission list can
// have. A list is only worth keeping while it stays true.
//
// CASE AND STYLE come from `docs/VOTV_UI_STYLE.md`: the frame is the game's `inst_uiBorder`
// material over `#313131`, the text is `font_ui` at the donor's own size, and labels are
// never all-caps (structure rule 4).

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
//
// ONLY WHILE THE PARENT IS ALIVE -- `RemoveChild` is a ProcessEvent dispatch. Use
// `Release` below on the menu-instance death edge.
void Destroy(Field* f);

// EVERYTHING `Destroy` DOES EXCEPT TOUCHING THE ENGINE: unhook the focus, drop the
// registry row, free the handle. For a tree that died with its `ui_menu_C` instance.
//
// The distinction is not theoretical. `browser_input_screens` called `Destroy` on the
// menu-instance death edge -- two lines under its own comment saying "the widgets died
// with the menu instance" -- so it dispatched `RemoveChild` into a freed tree, and
// `reflection::CallFunction` has no liveness check. A reused GUObjectArray slot makes that
// a call against an unrelated object rather than a clean fault, which is this project's
// "a wrong-offset write never faults where you wrote it" class. Both sibling screens
// already drop their pointers and touch nothing on that edge; this is what lets a field do
// the same. Found by the post-ship correctness audit, 2026-08-31.
void Release(Field* f);

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

// True ONCE after Escape was pressed while this field held focus -- meaning the field
// ATE that Escape to leave itself, and the owning screen must NOT also act on it.
//
// The owner used to ask `AnyFocused()` instead, and it could never work: the blur happens
// on WM_KEYDOWN and the screens take their edge on the key-UP, so the field had always
// let go by the time they asked. One press both blurred the field AND closed the window,
// discarding whatever had been typed (post-ship audit, 2026-08-31). This latch is set by
// the same event that consumes the key, so there is no ordering to get wrong.
bool ConsumeEscape(Field* f);

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
