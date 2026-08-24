// ue_wrap/store_catalog.cpp -- see ue_wrap/world/store_catalog.h.
//
// Build = walk `UDataTable::RowMap` for {row name -> live row bytes, price}, then verify every price
// against `GetDataTableColumnAsString`, which is fully reflected and needs no layout. One
// disagreement invalidates the whole catalog. All offsets resolved BY NAME off the table's RowStruct.

#include "ue_wrap/world/store_catalog.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdlib>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace ue_wrap::store_catalog {
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
// MEASURED correct on this build by store_table_probe (all four digests matched); it is also the
// only layout assumption in this file, and the gate below is what catches it if a recook moves it.
constexpr int32_t kElemStride = 24;
constexpr int32_t kElemRowPtr = 8;

// A row count outside this is not the shop table; refuse rather than walk it.
constexpr int32_t kSaneRowCap = 100000;

ue_wrap::CachedObjRef g_table;      // the UDataTable; a cooked asset, so world-stamp-exempt
std::unordered_map<std::wstring, Row> g_rows;
int32_t g_subcatOff = -1;
int32_t g_nameOff   = -1;
bool    g_built     = false;  // a build ATTEMPT has completed (success or failure)
bool    g_valid     = false;  // ...and it produced a usable catalog

// Lowercase for the key, because FName comparison is case-insensitive and a caller holding a name
// read back from an FName must find the row regardless of the display casing.
std::wstring Key(const std::wstring& s) {
    std::wstring k;
    k.reserve(s.size());
    for (wchar_t c : s) k.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c);
    return k;
}

// Drill knob: read a NEIGHBOURING field instead of `price`, so the gate below disagrees and the
// fail-closed path can be shown RED. Without this the refusal branch can never fire in a healthy
// build, which makes it an instrument that always passes
// ([[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]).
bool BreakDrillEnabled() {
    wchar_t buf[8]{};
    const DWORD n = ::GetEnvironmentVariableW(L"VOTVCOOP_STORE_CATALOG_BREAK", buf, 8);
    return n == 1 && buf[0] == L'1';
}

void* ResolveTable() {
    if (g_table.Alive()) return g_table.Raw();
    g_table.Set(R::FindObject(L"list_store", L"DataTable"));
    return g_table.Raw();
}

// The gate: the whole `price` column via the reflected getter. Empty on any failure, which the
// caller treats as "cannot verify" -> catalog invalid. ONE ProcessEvent dispatch for all 473 rows.
bool ReadPriceColumn(void* table, const std::wstring& propName, std::vector<int32_t>& out) {
    out.clear();
    void* cdo = R::FindClassDefaultObject(L"DataTableFunctionLibrary");
    void* fn  = cdo ? R::FindFunction(R::ClassOf(cdo), L"GetDataTableColumnAsString") : nullptr;
    if (!cdo || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (!f.Set<void*>(L"DataTable", table)) return false;
    if (!f.Set<R::FName>(L"PropertyName", ue_wrap::fname_utils::StringToFName(propName))) return false;
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
        out.push_back((s->Data && s->Num > 0) ? static_cast<int32_t>(::_wtoi(s->Data)) : 0);
        if (s->Data) R::EngineFree(s->Data);
    }
    R::EngineFree(arr.Data);
    return true;
}

// Build once. Any failure leaves g_valid false and is LOUD: this module's whole job is to be the
// thing a charge is derived from, so "quietly degraded" is not an available state.
void Build() {
    g_built = true;
    g_valid = false;
    g_rows.clear();
    g_subcatOff = -1;
    g_nameOff   = -1;

    void* table = ResolveTable();
    if (!table) return;  // not loaded yet -- Ready() retries (see the g_built reset there)

    void* dtCls = R::ClassOf(table);
    const int32_t offRowStruct = dtCls ? R::FindPropertyOffset(dtCls, L"RowStruct") : -1;
    if (offRowStruct < 0) {
        UE_LOGE("store_catalog: UDataTable::RowStruct did not resolve -- catalog INVALID");
        return;
    }
    void* rowStruct = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(table) + offRowStruct);
    if (!rowStruct) {
        UE_LOGE("store_catalog: list_store has no RowStruct -- catalog INVALID");
        return;
    }

    const int32_t offPrice  = R::FindPropertyOffsetByPrefix(rowStruct, L"price_");
    const int32_t offName   = R::FindPropertyOffsetByPrefix(rowStruct, L"name_");
    const int32_t offSize   = R::FindPropertyOffsetByPrefix(rowStruct, L"size_");
    const int32_t offSubcat = R::FindPropertyOffsetByPrefix(rowStruct, L"subcategory_");
    if (offPrice < 0 || offSubcat < 0 || offName < 0) {
        UE_LOGE("store_catalog: row struct members did not resolve (price@%d subcategory@%d "
                "name@%d) -- catalog INVALID", offPrice, offSubcat, offName);
        return;
    }

    // The drill deliberately reads `size` where `price` belongs. Both are int32 members of the same
    // struct, so this is a REALISTIC wrong-offset, not a nonsense one -- exactly the failure the gate
    // has to catch. It refuses to arm if `size` did not resolve, so the drill cannot silently no-op.
    const bool drill = BreakDrillEnabled();
    if (drill && offSize < 0) {
        UE_LOGE("store_catalog: BREAK drill requested but `size_` did not resolve -- refusing to "
                "arm a drill that would not actually corrupt the read");
        return;
    }
    const int32_t readOff = drill ? offSize : offPrice;
    if (drill)
        UE_LOGW("store_catalog: VOTVCOOP_STORE_CATALOG_BREAK=1 -- reading `size` (@%d) where `price` "
                "(@%d) belongs; the gate below MUST reject this", offSize, offPrice);

    // ---- the walk: RowMap sits immediately after the reflected RowStruct -------------------------
    auto* rowMap = reinterpret_cast<uint8_t*>(table) + offRowStruct + sizeof(void*);
    const TArrayRaw elems = *reinterpret_cast<TArrayRaw*>(rowMap);
    if (!elems.Data || elems.Num <= 0 || elems.Num > kSaneRowCap) {
        UE_LOGE("store_catalog: RowMap element array head is not plausible (data=%p num=%d) "
                "-- catalog INVALID", elems.Data, elems.Num);
        return;
    }

    std::vector<int32_t> walkPrices;
    walkPrices.reserve(static_cast<size_t>(elems.Num));
    for (int32_t i = 0; i < elems.Num; ++i) {
        auto* e = reinterpret_cast<uint8_t*>(elems.Data) + static_cast<size_t>(i) * kElemStride;
        auto* rowPtr = *reinterpret_cast<uint8_t**>(e + kElemRowPtr);
        if (!rowPtr) {
            UE_LOGE("store_catalog: RowMap element %d has a null row -- catalog INVALID", i);
            return;
        }
        Row r;
        r.data  = rowPtr;
        r.price = *reinterpret_cast<const int32_t*>(rowPtr + readOff);
        walkPrices.push_back(r.price);
        g_rows.emplace(Key(R::ToString(*reinterpret_cast<R::FName*>(e))), r);
    }
    if (g_rows.size() != static_cast<size_t>(elems.Num)) {
        UE_LOGE("store_catalog: %d rows collapsed to %zu keys -- duplicate row names, catalog "
                "INVALID", elems.Num, g_rows.size());
        g_rows.clear();
        return;
    }

    // ---- the gate: the same prices, read a completely different way ------------------------------
    // Tried with the BP-mangled member name first and the friendly name second; the probe measured
    // that the engine accepts EITHER, and trying both means a change in which one it honours degrades
    // to a slower path rather than to a false INVALID.
    std::wstring mangled;
    for (const auto& fld : R::EnumerateStructFields(rowStruct))
        if (fld.name.rfind(L"price_", 0) == 0) { mangled = fld.name; break; }

    std::vector<int32_t> colPrices;
    if ((mangled.empty() || !ReadPriceColumn(table, mangled, colPrices)) &&
        !ReadPriceColumn(table, L"price", colPrices)) {
        UE_LOGE("store_catalog: the price column could not be read, so the walk cannot be verified "
                "-- catalog INVALID (refusing to price orders off an unverified read)");
        g_rows.clear();
        return;
    }
    if (colPrices.size() != walkPrices.size()) {
        UE_LOGE("store_catalog: gate length mismatch -- walk=%zu column=%zu -- catalog INVALID",
                walkPrices.size(), colPrices.size());
        g_rows.clear();
        return;
    }
    for (size_t i = 0; i < colPrices.size(); ++i) {
        if (colPrices[i] != walkPrices[i]) {
            UE_LOGE("store_catalog: gate DISAGREES at row %zu (walk=%d column=%d) -- the RowMap "
                    "layout assumption is wrong on this build; catalog INVALID, client orders will "
                    "be refused rather than mischarged", i, walkPrices[i], colPrices[i]);
            g_rows.clear();
            return;
        }
    }

    g_subcatOff = offSubcat;
    g_nameOff   = offName;
    g_valid     = true;
    int64_t sum = 0;
    for (int32_t p : walkPrices) sum += p;
    UE_LOGI("store_catalog: %zu rows, price sum %lld, verified against the reflected price column "
            "(subcategory@%d)", g_rows.size(), static_cast<long long>(sum), g_subcatOff);
}

}  // namespace

bool Ready() {
    if (g_valid && g_table.Alive()) return true;
    if (g_built && g_table.Alive()) return false;  // built and rejected -- do not rebuild in a loop
    // Either never built, or the table went away (which for a cooked asset should not happen; if it
    // does, rebuilding against whatever replaced it is the honest response).
    Build();
    return g_valid;
}

const Row* Find(const std::wstring& rowName) {
    if (!Ready()) return nullptr;
    auto it = g_rows.find(Key(rowName));
    return (it == g_rows.end()) ? nullptr : &it->second;
}

int32_t Count() { return g_valid ? static_cast<int32_t>(g_rows.size()) : 0; }

int32_t SubcategoryOffset() { return g_valid ? g_subcatOff : -1; }

int32_t NameOffset() { return g_valid ? g_nameOff : -1; }

}  // namespace ue_wrap::store_catalog
