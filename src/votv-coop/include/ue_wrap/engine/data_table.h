// ue_wrap/engine/data_table.h -- a UDataTable's rows, read two ways: a raw walk of its RowMap, which
// alone yields the live row bytes, and the engine's reflected GetDataTableColumnAsString, which needs
// no layout and is how a reader verifies its walk. Engine-wrapper layer (principle 7): no network, no
// coop state, no policy. The one layout assumption, the RowMap element's stride and row-pointer
// offset, was measured on this build (coop/dev/store_table_probe); a reader's column gate catches it
// if a recook moves it. GetDataTableRowFromName cannot be called from C++: its CustomThunk compares
// the declared out-param struct against the table's RowStruct and bails. Game thread only.

#pragma once

#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::data_table {

// One RowMap entry: its key, and the live row, valid for as long as the table lives.
struct RowRef {
    reflection::FName key{};
    const uint8_t*    row = nullptr;
};

// The table's row struct (a UScriptStruct), or nullptr when RowStruct does not resolve or is unset.
void* RowStruct(void* table);

// Every entry of the table's RowMap, in storage order, which is the order a column read returns.
// False, with `out` empty and a line naming `who`, when the head is not plausible or a row is null.
bool Rows(void* table, std::vector<RowRef>& out, const char* who);

// A column read by the engine, one string per row in the same order. `member` may be the member's
// Blueprint-mangled name or its plain one; the engine accepts either. False on any failure.
bool ColumnAsStrings(void* table, const std::wstring& member, std::vector<std::wstring>& out);

// The Blueprint-mangled name of the row struct's member whose name starts with `prefix` ("price_"),
// or empty.
std::wstring MemberName(void* rowStruct, const wchar_t* prefix);

}  // namespace ue_wrap::data_table
