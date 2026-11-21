// coop/props/prop_act_lines.h -- the lines a client's refused or unasked take, use, put-on or take-off says (prop_act_request).
//
// Each kind of line is shown at most once per window, through the chat feed. Game thread.

#pragma once

#include <cstdint>

namespace coop::prop_act_lines {

void ShowRefusal(uint8_t reason);   // the line of a PropActReason; none for a reason that has no line
void ShowBadReason();
void ShowNotAsked();
void ShowNotReady();
void ShowBusy();

void Forget();                      // the session ended: every window is open again
bool NotReadyShown();               // dev: the not-ready line has been said
}  // namespace coop::prop_act_lines
