// ue_wrap/ftext_utils.h -- a VALID empty engine FText for safe hand-built struct construction.
//
// Engine-wrapper layer (principle 7); sibling of ue_wrap/fname_utils. No gameplay/network logic.
//
// WHY THIS EXISTS. When we hand-build a UE struct in C++ that carries an FText member and then
// pass it to a native UFunction (e.g. Fstruct_store.subcategory @0x28 inside the order we feed to
// Uui_laptop_C::makeAnOrder), the FText slot can NOT simply be left zeroed. A zeroed FText is a
// null TSharedRef: copy/destruct are null-guarded so the array-copy in addOrderCart won't crash,
// but ANY deref (the host opening the laptop "orders" tab -> genStoreCart renders subcategory)
// dereferences the null text data and crashes. So the slot must hold a real, valid empty FText.
//
// We mint ONE via UKismetTextLibrary::Conv_StringToText("") and PIN it: ParamFrame raw-frees its
// frame WITHOUT UE-destructing OUT params (see ue_wrap/call.h), so the returned FText's +1 ref is
// never released -> the underlying (empty) FTextData stays live process-wide -> the cached 0x18
// bytes remain valid forever. Reusing those bytes as a raw byte-copy into N struct slots is safe:
// a byte-copy adds no refcount, the native deep-copy (addOrderCart's Array_Add) adds the proper
// per-element refs, and the native drain at delivery releases them -- balanced (and a no-op if the
// empty maps to FText::GetEmpty()'s immortal shared data).

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::ftext_utils {

// sizeof(FText) in UE4.27 (TSharedRef<ITextData> + uint32 flags, padded). Matches the
// Fstruct_store.subcategory size in the SDK dump (struct_store.hpp: 0x18).
inline constexpr int kFTextSize = 0x18;

// Copy the bytes of a valid empty engine FText into `out` (must be >= kFTextSize bytes).
// Lazily mints + pins the empty FText on first call; subsequent calls memcpy the cache.
// Returns false (out untouched) if the Kismet text library isn't resolvable yet (still
// booting / at the menu). Game-thread only (ProcessEvent dispatch on first mint).
bool EmptyFText(void* out);

// Mint a live FText from `s` via UKismetTextLibrary::Conv_StringToText and copy its
// 0x18 bytes into `out`. The mint's +1 ref deliberately leaks (the ParamFrame raw-free
// mechanism documented above) so the copied bytes stay valid for the consumer's own
// deep-copy -- negligible at human event rates (v64 email mirror). Game thread.
bool MintFText(const wchar_t* s, void* out);

// The inverse: read an engine FText's display string via
// UKismetTextLibrary::Conv_TextToString. `ftext` points at 0x18 FText bytes (e.g. a
// struct member on a live object). Empty on failure / unresolved. Game thread.
std::wstring FTextToString(const void* ftext);

}  // namespace ue_wrap::ftext_utils
