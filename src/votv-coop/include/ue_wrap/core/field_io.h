// ue_wrap/core/field_io.h -- raw UObject field readers/writers for FString /
// TArray<FString> / TArray<int32> slots, with the swap-and-EngineFree doctrine.
//
// The doctrine: the fstring_utils PIN rule ("leak it, the engine's later reassign frees it")
// holds only for FRESH buffers. Device fields are overwritten REPEATEDLY on live instances,
// with no native reassign between our writes on a non-presser peer -- so WE free what WE
// replaced (EngineFree is GMalloc-matched).
//
// Game thread only. No network logic, no coop state (principle 7).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::field_io {

struct FStringView { wchar_t* data; int32_t num; int32_t max; };
struct TArrayView  { uint8_t* data; int32_t num; int32_t max; };

// Read the FString at (base + off). Empty on null/short.
std::wstring ReadFStringAt(const void* base, int32_t off);

// Overwrite the FString slot: mint fresh, free the replaced buffer.
// False (old value intact) on mint failure or off < 0.
bool WriteFStringField(void* base, int32_t off, const std::wstring& s);

// TArray<FString> read/write (16 B per element header).
std::vector<std::wstring> ReadFStringArrayField(const void* base, int32_t off);
bool WriteFStringArrayField(void* base, int32_t off, const std::vector<std::wstring>& in);

// TArray<int32> read/write.
std::vector<int32_t> ReadInt32ArrayField(const void* base, int32_t off);
bool WriteInt32ArrayField(void* base, int32_t off, const std::vector<int32_t>& in);

}  // namespace ue_wrap::field_io
