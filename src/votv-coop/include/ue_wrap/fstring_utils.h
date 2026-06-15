// ue_wrap/fstring_utils.h -- engine-allocated FString construction.
//
// A param-frame FString may point at OUR buffer for a call's duration (the
// callee deep-copies; the economy order-struct precedent). A LIVE STRUCT
// FIELD is different: the game later copy-assigns/destroys that field with
// the ENGINE allocator (e.g. comp_uploadData's eject swaps comp_data_0 whole)
// -- a buffer we malloc'd would be FMemory::Free'd, an allocator-mismatch
// heap corruption. So in-place FString writes mint the buffer ENGINE-SIDE:
// one reflected UKismetStringLibrary::Concat_StrStr("", s) returns an FString
// whose data the engine allocated; we memcpy the 16-byte header into the
// field and deliberately never free it ourselves (the ftext_utils pin/leak
// doctrine -- the engine's later reassign/destroy frees it safely).
//
// Game thread only.

#pragma once

#include "ue_wrap/reflection.h"

#include <string>

namespace ue_wrap::fstring_utils {

// Mint an engine-owned FString holding `s` and write its {Data,Num,Max}
// header to `outHeader16` (an FString-typed field slot). Empty input writes
// the null FString {nullptr,0,0} (valid empty). False on resolve failure.
bool MintFString(const std::wstring& s, void* outHeader16);

}  // namespace ue_wrap::fstring_utils
