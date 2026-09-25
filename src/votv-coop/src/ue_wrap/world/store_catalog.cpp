// ue_wrap/world/store_catalog.cpp -- see ue_wrap/world/store_catalog.h.
//
// Build = walk `UDataTable::RowMap` for {row name -> live row bytes, price}, then verify every price
// against `GetDataTableColumnAsString`, which is fully reflected and needs no layout
// (ue_wrap/engine/data_table reads both). One disagreement invalidates the whole catalog. All
// offsets resolved BY NAME off the table's RowStruct.

#include "ue_wrap/world/store_catalog.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/data_table.h"

#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace ue_wrap::store_catalog {
namespace {

namespace R  = ue_wrap::reflection;
namespace DT = ue_wrap::data_table;

ue_wrap::CachedObjRef g_table;      // the UDataTable; a cooked asset, so world-stamp-exempt
std::unordered_map<std::wstring, Row> g_rows;
int32_t g_subcatOff = -1;
int32_t g_nameOff   = -1;
int32_t g_objectOff = -1;  // optional: a row item never needs it, an unnamed one does
int32_t g_sizeOff   = -1;
bool    g_valid     = false;  // a build produced a usable catalog

// A failed build is not one thing, and treating it as one is wrong in BOTH directions:
//   HARD -- a verdict about LAYOUT or DATA (the price gate disagreed, duplicate row keys,
//           row-struct members missing). Retrying cannot change the answer, so latch it forever.
//   SOFT -- something was not resolvable YET (the table, the function library, the RowMap head).
//           Retrying is right, but the retry runs FindObject, a full GUObjectArray walk that
//           renders an FName per object; with no latch at all on this path a host with a pending
//           order can re-walk the whole array once per order per frame. So: retry, on a
//           wall-clock throttle.
// Latching a SOFT failure permanently is the opposite error -- it refuses every client order for
// the rest of the session because a UFunction happened to be unresolved for one tick.
enum class Outcome { Never, Soft, Hard };
Outcome  g_outcome      = Outcome::Never;
uint64_t g_lastAttemptMs = 0;
constexpr uint64_t kRebuildThrottleMs = 3000;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Lowercase for the key, because FName comparison is case-insensitive and a caller holding a name
// read back from an FName must find the row regardless of the display casing.
std::wstring Key(const std::wstring& s) {
    std::wstring k;
    k.reserve(s.size());
    for (wchar_t c : s) k.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c);
    return k;
}

// Drill knob: read a NEIGHBOURING field instead of `price`, so the gate below disagrees and the
// fail-closed path can be shown RED. Without it the refusal branch can never fire in a healthy
// build, leaving an instrument that cannot see the failure it guards and therefore always passes.
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
    std::vector<std::wstring> column;
    if (!DT::ColumnAsStrings(table, propName, column)) return false;
    out.reserve(column.size());
    for (const std::wstring& c : column) out.push_back(c.empty() ? 0 : static_cast<int32_t>(::_wtoi(c.c_str())));
    return true;
}

// Build once. Any failure leaves g_valid false and is LOUD: this module's whole job is to be the
// thing a charge is derived from, so "quietly degraded" is not an available state.
void Build() {
    g_valid = false;
    g_outcome = Outcome::Soft;  // upgraded to Hard by a verdict, cleared to Never on success
    g_lastAttemptMs = NowMs();
    g_rows.clear();
    g_subcatOff = -1;
    g_nameOff   = -1;
    g_objectOff = -1;
    g_sizeOff   = -1;

    void* table = ResolveTable();
    if (!table) return;  // not loaded yet -- SOFT, so Ready() retries on the throttle

    void* rowStruct = DT::RowStruct(table);
    if (!rowStruct) {
        UE_LOGE("store_catalog: list_store's RowStruct did not resolve -- catalog INVALID");
        return;  // SOFT: a reflection miss, not a verdict about the data
    }

    const int32_t offPrice  = R::FindPropertyOffsetByPrefix(rowStruct, L"price_");
    const int32_t offName   = R::FindPropertyOffsetByPrefix(rowStruct, L"name_");
    const int32_t offSize   = R::FindPropertyOffsetByPrefix(rowStruct, L"size_");
    const int32_t offSubcat = R::FindPropertyOffsetByPrefix(rowStruct, L"subcategory_");
    const int32_t offObject = R::FindPropertyOffsetByPrefix(rowStruct, L"object_");
    if (offPrice < 0 || offSubcat < 0 || offName < 0) {
        UE_LOGE("store_catalog: row struct members did not resolve (price@%d subcategory@%d "
                "name@%d) -- catalog INVALID", offPrice, offSubcat, offName);
        g_outcome = Outcome::Hard;  // the struct is not what we think it is; retrying won't help
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

    // ---- the walk -------------------------------------------------------------------------------
    std::vector<DT::RowRef> refs;
    if (!DT::Rows(table, refs, "store_catalog")) return;  // SOFT, and said by the walk: catalog INVALID

    std::vector<int32_t> walkPrices;
    walkPrices.reserve(refs.size());
    for (const DT::RowRef& ref : refs) {
        Row r;
        r.data  = ref.row;
        r.price = *reinterpret_cast<const int32_t*>(ref.row + readOff);
        r.key   = ref.key;  // kept: the commit stamps it, no re-mint needed
        walkPrices.push_back(r.price);
        g_rows.emplace(Key(R::ToString(r.key)), r);
    }
    if (g_rows.size() != refs.size()) {
        UE_LOGE("store_catalog: %zu rows collapsed to %zu keys -- duplicate row names, catalog "
                "INVALID", refs.size(), g_rows.size());
        g_rows.clear();
        g_outcome = Outcome::Hard;  // a fact about the table's data
        return;
    }

    // ---- the gate: the same prices, read a completely different way ------------------------------
    // Tried with the BP-mangled member name first and the friendly name second; the probe measured
    // that the engine accepts EITHER, and trying both means a change in which one it honours degrades
    // to a slower path rather than to a false INVALID.
    const std::wstring mangled = DT::MemberName(rowStruct, L"price_");

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
            g_outcome = Outcome::Hard;  // a LAYOUT verdict -- never retry, never guess
            return;
        }
    }

    g_subcatOff = offSubcat;
    g_nameOff   = offName;
    g_objectOff = offObject;
    g_sizeOff   = offSize;
    g_valid     = true;
    g_outcome   = Outcome::Never;
    int64_t sum = 0;
    for (int32_t p : walkPrices) sum += p;
    UE_LOGI("store_catalog: %zu rows, price sum %lld, verified against the reflected price column "
            "(subcategory@%d object@%d size@%d)", g_rows.size(), static_cast<long long>(sum), g_subcatOff,
            g_objectOff, g_sizeOff);
}

}  // namespace

bool Ready() {
    if (g_valid && g_table.Alive()) return true;
    if (g_outcome == Outcome::Hard) return false;  // a verdict; retrying cannot change it
    if (g_outcome == Outcome::Soft && NowMs() - g_lastAttemptMs < kRebuildThrottleMs)
        return false;  // the retry is a full GUObjectArray walk -- do not run it per call
    Build();
    return g_valid;
}

const Row* Find(const std::wstring& rowName) {
    if (!Ready()) return nullptr;
    auto it = g_rows.find(Key(rowName));
    return (it == g_rows.end()) ? nullptr : &it->second;
}

int32_t SubcategoryOffset() { return Ready() ? g_subcatOff : -1; }

int32_t NameOffset() { return Ready() ? g_nameOff : -1; }

int32_t ObjectOffset() { return Ready() ? g_objectOff : -1; }

int32_t SizeOffset() { return Ready() ? g_sizeOff : -1; }

}  // namespace ue_wrap::store_catalog
