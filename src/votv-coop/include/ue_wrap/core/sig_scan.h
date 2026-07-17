// ue_wrap/sig_scan.h -- AOB (array-of-bytes) signature scanner.
//
// Engine-wrapper layer (principle 7): no gameplay/network logic. Scans the
// main module image for a byte pattern so the standalone mod can resolve
// engine globals/functions itself, with NO UE4SS at runtime (RULE No.3).
//
// Pattern syntax: space-separated hex bytes, "??" (or "?") = wildcard.
// e.g. "48 8B ?? ?? 89 01".

#pragma once

#include <cstdint>

namespace ue_wrap {

// Scans the primary executable module (GetModuleHandle(nullptr)) for `pattern`.
// Returns the address of the first match, or 0 if not found.
uintptr_t FindPattern(const char* pattern);

// Same, but scans an explicit [base, base+size) range.
uintptr_t FindPatternIn(uintptr_t base, size_t size, const char* pattern);

// [base, size) of the main executable image, from its PE headers.
void MainModuleRange(uintptr_t& base, size_t& size);

}  // namespace ue_wrap
