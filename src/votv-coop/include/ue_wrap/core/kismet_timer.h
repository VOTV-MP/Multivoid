// ue_wrap/core/kismet_timer.h -- a Blueprint's timers by object and function name.
//
// KismetSystemLibrary's K2_ClearTimer, which builds the delegate from the two and so answers the blueprint's own
// K2_SetTimerDelegate({self, name}) arm. The library's default object is a whole-array walk to find and never goes away, so it
// and the function are found once here and kept. The name crosses as an FString whose count and capacity include the
// terminator. Game thread.

#pragma once

namespace ue_wrap::kismet_timer {

// Whether the library is found, resolving it if not. Every call retries until it is found, so one caller's failure never
// reads as another's; only the warning for a failure is limited, to once a second.
bool Resolved();

// Resolves the library itself on first use, so no caller needs Resolved() first. False when the library is not found or the
// call did not dispatch. A timer the object does not have is no failure.
bool Clear(void* object, const wchar_t* function);

}  // namespace ue_wrap::kismet_timer
