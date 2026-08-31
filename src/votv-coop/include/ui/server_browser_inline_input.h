// ui/server_browser_inline_input.h -- VARIANT B of the browser's input fork: the address
// and the nickname are typed IN the browser, in the right-hand column, under the panes.
//
// THE CASE THIS VARIANT HAS TO MAKE. The user's objection to inline input was specific and
// it was earned: the boxes that shipped for one build sat in a footer bar, the address
// clipped its own text as it was typed, and neither was labelled -- "ебаный текст в ебаное
// окно ввода ip не помещается ... это дизайн говно". Their veto was CONDITIONAL, against
// inline input *if it is crutchy and pointless by design*, and the instruction was to build
// both and choose by eye. So this is not the same thing again in the same place: the fields
// are labelled, they live in the column that already answers "who you are and what you are
// looking at" rather than crowding the list, the overflow defect is fixed in the primitive
// underneath them (an alignment flip -- the tail and the caret stay visible at any length),
// and every one of them is a framed cell of the game's own shape.
//
// WHAT IT COSTS, stated because it is the argument AGAINST this variant: two fields are
// always on screen and always focusable, so the browser owns keyboard state whenever it is
// open. Variant A's windows own the keyboard only while they exist. That is the trade the
// user is being shown, and it is a matter of taste rather than of correctness -- which is
// why it is being settled by eye and not here.
//
// Game thread only.

#pragma once

namespace ui::server_browser_inline_input {

// Is this variant in force (config `ui.browser_inline_input`)? Latched -- it decides which
// widgets are BUILT, so a mid-session change would leave the layout disagreeing with the
// input router.
bool Armed();

// Build the two labelled field rows into `parent` (the browser's right-hand UVerticalBox),
// styled from `donorBtn`. A no-op returning true when this variant is not armed, so the
// browser's build path reads the same either way. False only on a real build failure.
bool Build(void* parent, void* donorBtn);

// Per-tick: drives both fields (caret, click-to-focus) and applies a name the player has
// finished editing. Cheap and safe when nothing was built.
void Tick();

// A left-button RELEASE landed while the browser is open, and neither the chrome nor the
// action grid claimed it. Returns true if this variant consumed it.
bool OnReleaseEdge();

// The ADDRESS the player has typed, for the action grid's "Direct connect" cell -- which is
// the same cell variant A uses to open a window, pointed at this field instead.
const char* Address();

// DID THE PLAYER PRESS ENTER IN THE ADDRESS FIELD. Consumed once, like the field's own
// submit edge it rides on.
//
// The address commits on Enter and NOT on losing focus, unlike the name beside it, and the
// asymmetry is deliberate: a name is a SETTING and leaving the box means you are done with
// it, while an address is an ACTION and clicking away from it must not dial anything.
bool ConsumeAddressSubmit();

// The menu instance died and took the widgets with it.
void Forget();

}  // namespace ui::server_browser_inline_input
