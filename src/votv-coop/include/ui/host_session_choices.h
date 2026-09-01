// ui/host_session_choices.h -- the two-row CHOICE selector the session-settings screen is
// built out of, and the one place its row kit lives.
//
// WHY THIS IS A MODULE AND NOT A THIRD COPY. `host_session_settings.cpp` grew two of these
// by hand -- WHO MAY JOIN and SERVER LIST -- as structurally identical blocks: a heading, a
// const table of two {title, detail} answers, two hand-built `UImage` rows, a chosen index,
// a hover index, an editability predicate, and a repaint that drives TWO independent style
// channels. The connection-type rework adds a THIRD (who may connect), and the kit's own
// rule -- no new shared framework before three working cases (OPUS_48_DISCIPLINE:196) -- is
// satisfied exactly here. A third copy would also have been the third place to get the
// two-channel style wrong, and the second place to re-derive that a hand-built `UImage`
// answers `IsHovered()` with 0 and must be hit-tested by GEOMETRY.
//
// THE STYLE IS `docs/VOTV_UI_STYLE.md` SECTION 4 AND IS NOT A CHOICE THIS MODULE MAKES:
// selection is a row FILL change, hover is a TEXT colour change, and they are independent.
// Porting ImGui's HeaderHovered (one channel, fill-on-hover) would look foreign in VOTV's
// own menus, which is measured, not taste.
//
// Game thread only, like every other native-screen module.

#pragma once

namespace ui::host_session_choices {

// One answer to one question. Both strings are compile-time literals owned by the caller:
// the wording is product surface and belongs beside the question it answers, not here.
struct Answer {
    const wchar_t* title;
    const wchar_t* detail;
};

// A built selector. The caller keeps one of these per question and hands it back on every
// repaint / hover / click, so this module holds no per-question state of its own and two
// screens can own selectors at once without a registry.
//
// `chosen` is an INDEX rather than a bool because a question with two answers today is a
// question with three tomorrow, and every read site would otherwise encode the arity.
struct Selector {
    void* bg[2]    = {nullptr, nullptr};   // the hit targets, and the fill channel
    void* title[2] = {nullptr, nullptr};   // the text channel
    int   chosen   = 0;
    int   hover    = -1;
    // CAN THE PLAYER MOVE IT AT ALL. A selector that is fixed by something else still
    // STATES THE TRUTH -- it keeps its selection fill and loses the white -- rather than
    // disappearing, because a control that vanishes teaches nothing and a control that
    // ignores clicks teaches the wrong thing.
    bool  editable = true;
};

// Build the heading and the two rows into `column`. Returns false if any widget failed, and
// the caller must treat that as a build failure: a half-built selector is a question with
// one visible answer.
//
// The caller owns the failure path. This does NOT clear `out` on failure -- the screen's own
// teardown does that, and a module that half-cleaned would hide which widget was missing.
bool Build(void* column, const wchar_t* heading, const Answer (&answers)[2], Selector& out);

// Apply both style channels for the current `chosen` / `hover` / `editable`.
void Repaint(const Selector& s);

// Which row the cursor is over, or -1. `hx`/`hy` are cursor-in-widget-space, resolved ONCE
// by the caller for the whole sweep: the conversion reaches an uncached GUObjectArray walk,
// so resolving it per row cost one walk per row per tick.
//
// Answers -1 for a non-editable selector by design -- lighting up a row the player cannot
// move promises a control that is not there.
int HoverAt(const Selector& s, long hx, long hy);

// Move the selection if the cursor is over a row. Returns true if this selector consumed the
// click, whether or not the value changed -- a click that lands on the already-chosen row is
// still this selector's click and must not fall through to another one.
bool HandleClick(Selector& s, long hx, long hy, int& outChosen);

// Drop every widget pointer. The VALUES (`chosen`) survive on purpose: they are the player's
// answers, and a menu-instance rebuild must not silently reset a question they already
// answered.
void ClearWidgets(Selector& s);

}  // namespace ui::host_session_choices
