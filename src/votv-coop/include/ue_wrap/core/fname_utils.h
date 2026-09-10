// ue_wrap/core/fname_utils.h -- engine FName construction from wide strings.
//
// It lives here rather than with the prop drive state machine it was cut
// from: the conceptual home of an FName helper is engine substrate.
// Pure engine helper; no gameplay/network logic (principle 7).

#pragma once

#include "ue_wrap/core/reflection.h"

#include <string>

namespace ue_wrap::fname_utils {

// Construct a live engine FName from a wide string by dispatching
// UKismetStringLibrary::Conv_StringToName(FString) -> FName via ProcessEvent.
// Returns NAME_None ({0,0}) on failure or empty input. The string is
// interned in the global FName pool with a fresh local index; subsequent
// FName equality compares against the same string will hit this index.
//
// Lazy-resolves the Kismet CDO + UFunction + param-name on first call.
// Game-thread only (ProcessEvent dispatch invariant).
reflection::FName StringToFName(const std::wstring& s);

}  // namespace ue_wrap::fname_utils
