// ue_wrap/engine/data_table.cpp -- see ue_wrap/engine/data_table.h.

#include "ue_wrap/engine/data_table.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"

namespace ue_wrap::data_table {
namespace {

namespace R = ue_wrap::reflection;

// UE4 TArray head -- also the head of TSparseArray::Data, which is what RowMap's element storage is.
struct TArrayRaw {
    void*   Data;
    int32_t Num;
    int32_t Max;
};

// UE4 FString == TArray<TCHAR>.
struct FStringRaw {
    wchar_t* Data;
    int32_t  Num;
    int32_t  Max;
};

// TSetElement<TTuple<FName, uint8*>> = {FName key; uint8* row; int32 HashNextId; int32 HashIndex}.
// MEASURED correct on this build by store_table_probe (all four digests matched).
constexpr int32_t kElemStride = 24;
constexpr int32_t kElemRowPtr = 8;

// A row count outside this is not a table the game ships; refuse rather than walk it.
constexpr int32_t kSaneRowCap = 100000;

int32_t RowStructOffset(void* table) {
    void* cls = table ? R::ClassOf(table) : nullptr;
    return cls ? R::FindPropertyOffset(cls, L"RowStruct") : -1;
}

}  // namespace

void* RowStruct(void* table) {
    const int32_t off = RowStructOffset(table);
    if (off < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(table) + off);
}

bool Rows(void* table, std::vector<RowRef>& out, const char* who) {
    out.clear();
    const int32_t off = RowStructOffset(table);
    if (off < 0) {
        UE_LOGE("%s: UDataTable::RowStruct did not resolve, so the RowMap cannot be found", who);
        return false;
    }
    // RowMap sits immediately after the reflected RowStruct.
    const TArrayRaw elems =
        *reinterpret_cast<const TArrayRaw*>(reinterpret_cast<uint8_t*>(table) + off + sizeof(void*));
    if (!elems.Data || elems.Num <= 0 || elems.Num > kSaneRowCap) {
        UE_LOGE("%s: RowMap element array head is not plausible (data=%p num=%d)", who, elems.Data, elems.Num);
        return false;
    }
    out.reserve(static_cast<size_t>(elems.Num));
    for (int32_t i = 0; i < elems.Num; ++i) {
        const auto* e = reinterpret_cast<const uint8_t*>(elems.Data) + static_cast<size_t>(i) * kElemStride;
        const auto* row = *reinterpret_cast<const uint8_t* const*>(e + kElemRowPtr);
        if (!row) {
            UE_LOGE("%s: RowMap element %d has a null row", who, i);
            out.clear();
            return false;
        }
        out.push_back({*reinterpret_cast<const R::FName*>(e), row});
    }
    return true;
}

bool ColumnAsStrings(void* table, const std::wstring& member, std::vector<std::wstring>& out) {
    out.clear();
    void* cdo = R::FindClassDefaultObject(L"DataTableFunctionLibrary");
    void* fn  = cdo ? R::FindFunction(R::ClassOf(cdo), L"GetDataTableColumnAsString") : nullptr;
    if (!cdo || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (!f.Set<void*>(L"DataTable", table)) return false;
    if (!f.Set<R::FName>(L"PropertyName", ue_wrap::fname_utils::StringToFName(member))) return false;
    if (!ue_wrap::Call(cdo, f)) return false;
    TArrayRaw arr{nullptr, 0, 0};
    if (!f.GetRaw(L"ReturnValue", &arr, sizeof(arr))) return false;
    if (!arr.Data || arr.Num <= 0 || arr.Num > kSaneRowCap) {
        if (arr.Data) R::EngineFree(arr.Data);
        return false;
    }
    out.reserve(static_cast<size_t>(arr.Num));
    for (int32_t i = 0; i < arr.Num; ++i) {
        auto* s = reinterpret_cast<FStringRaw*>(reinterpret_cast<uint8_t*>(arr.Data) +
                                                static_cast<size_t>(i) * sizeof(FStringRaw));
        out.emplace_back((s->Data && s->Num > 0) ? std::wstring(s->Data) : std::wstring());
        if (s->Data) R::EngineFree(s->Data);
    }
    R::EngineFree(arr.Data);
    return true;
}

std::wstring MemberName(void* rowStruct, const wchar_t* prefix) {
    if (!rowStruct || !prefix) return {};
    const std::wstring p(prefix);
    for (const auto& fld : R::EnumerateStructFields(rowStruct))
        if (fld.name.rfind(p, 0) == 0) return fld.name;
    return {};
}

}  // namespace ue_wrap::data_table
