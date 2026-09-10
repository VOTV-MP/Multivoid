// ue_wrap/core/ftext_utils.h -- a VALID empty engine FText for safe hand-built struct construction.
// Engine-wrapper layer (principle 7), sibling of ue_wrap/fname_utils. No gameplay or network logic.
//
// WHY THIS EXISTS. When we hand-build a UE struct in C++ that carries an FText member and pass it
// to a native UFunction -- the subcategory inside the order we feed to Uui_laptop_C::makeAnOrder --
// the FText slot can NOT be left zeroed. A zeroed FText is a null TSharedRef whose copy and
// destruct are null-guarded, so addOrderCart's array copy survives it and the crash arrives later,
// when the host opens the laptop's orders tab and genStoreCart derefs subcategory. So the slot has
// to hold a real, valid, empty FText.
//
// We mint ONE through UKismetTextLibrary::Conv_StringToText("") and PIN it: ParamFrame raw-frees
// its frame WITHOUT UE-destructing OUT params (ue_wrap/core/call.h), so the returned FText's +1 ref
// is never released and the underlying empty FTextData stays live for the process. Byte-copying
// those bytes into struct slots is safe: the copy adds no refcount, addOrderCart's Array_Add deep
// copy adds the per-element refs, and the native drain at delivery releases them.

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

// Mint a live FText from `s` via UKismetTextLibrary::Conv_StringToText and copy its 0x18 bytes into
// `out`. The mint's +1 ref deliberately leaks, by the ParamFrame raw-free above, so the copied
// bytes stay valid for the consumer's own deep copy -- negligible at the rate a person generates
// these. Game thread.
bool MintFText(const wchar_t* s, void* out);

// The inverse: read an engine FText's display string via
// UKismetTextLibrary::Conv_TextToString. `ftext` points at 0x18 FText bytes (e.g. a
// struct member on a live object). Empty on failure / unresolved. Game thread.
std::wstring FTextToString(const void* ftext);

}  // namespace ue_wrap::ftext_utils
