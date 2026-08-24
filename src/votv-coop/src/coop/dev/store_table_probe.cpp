// coop/dev/store_table_probe.cpp -- see coop/dev/store_table_probe.h.
//
// One-shot, read-only, ini-gated. Runs three candidate `list_store` row readers and prints one
// comparable verdict line per candidate against the frozen offline truth, so the production reader
// is CHOSEN by measurement rather than by which engine-source recollection sounded right.

#include "coop/dev/store_table_probe.h"

#include "coop/config/config.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::store_table_probe {
namespace {

namespace R = ue_wrap::reflection;

// ---- the frozen offline truth (see the header for how it was produced) ----------------------
constexpr int32_t  kExpectRows       = 473;
constexpr int64_t  kExpectPriceSum   = 73271;
constexpr uint64_t kExpectDigest     = 0x7917FC66914020E1ULL;  // sorted "name=price\n"
constexpr uint64_t kExpectNameDigest = 0x3D110846BD629428ULL;  // sorted "name\n"

// A row count this far off means we resolved something that is not the shop table; refuse to walk it.
constexpr int32_t kSaneRowCap = 100000;

bool g_done = false;

// UE4 TArray head. Also the head of TSparseArray::Data, which is what UDataTable::RowMap's
// element storage is; candidate (c) reads it as such.
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

struct Row {
    std::string name;
    int32_t     price = 0;
    bool        havePrice = false;
};

uint64_t Fnv1a64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Row names are ASCII identifiers; lowercase so the digest cannot disagree with the offline one
// over FName display casing alone. A non-ASCII byte becomes '?', which would break the digest --
// deliberately, because a non-ASCII row name would mean we are not reading what we think we are.
std::string LowerNarrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        s.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    }
    return s;
}

// Print the one verdict line set for a candidate. `rows` is taken by value: it is sorted here, and
// sorting is part of the digest definition, not of any candidate's output.
void Verdict(const wchar_t* label, std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.name < b.name; });

    std::string blobNamePrice;
    std::string blobName;
    int64_t     sum        = 0;
    int32_t     withPrice  = 0;
    for (const Row& r : rows) {
        blobName += r.name;
        blobName += '\n';
        blobNamePrice += r.name;
        blobNamePrice += '=';
        blobNamePrice += std::to_string(r.price);
        blobNamePrice += '\n';
        if (r.havePrice) {
            ++withPrice;
            sum += r.price;
        }
    }
    const uint64_t dig  = Fnv1a64(blobNamePrice);
    const uint64_t digN = Fnv1a64(blobName);

    const int32_t n = static_cast<int32_t>(rows.size());
    UE_LOGI("store_table_probe: %ls -> rows=%d (%s) priced=%d sum=%lld (%s) "
            "digest=%016llX (%s) nameDigest=%016llX (%s)",
            label,
            n, (n == kExpectRows ? "MATCH" : "MISMATCH"),
            withPrice,
            static_cast<long long>(sum), (sum == kExpectPriceSum ? "MATCH" : "MISMATCH"),
            static_cast<unsigned long long>(dig),
            (dig == kExpectDigest ? "MATCH" : "MISMATCH"),
            static_cast<unsigned long long>(digN),
            (digN == kExpectNameDigest ? "MATCH" : "MISMATCH"));

    // A sample, so a MISMATCH is diagnosable without a second run.
    const size_t show = rows.size() < 6 ? rows.size() : 6;
    for (size_t i = 0; i < show; ++i)
        UE_LOGI("store_table_probe:   %ls[%zu] %hs = %d%s", label, i, rows[i].name.c_str(),
                rows[i].price, rows[i].havePrice ? "" : " (no price)");
}

void Unavailable(const wchar_t* label, const wchar_t* why) {
    UE_LOGI("store_table_probe: %ls -> UNAVAILABLE (%ls)", label, why);
}

// Log a UFunction's parameter chain. The param NAMES are an input to every candidate below, and an
// instrument that reports its inputs beats one that only reports a verdict
// ([[lesson-an-instrument-must-report-its-inputs-not-only-its-verdict]]).
void LogParams(const wchar_t* label, void* fn) {
    if (!fn) {
        UE_LOGI("store_table_probe: params %ls -> function not found", label);
        return;
    }
    std::wstring line;
    for (const auto& p : R::FunctionParams(fn)) {
        line += p.name;
        line += L"@" + std::to_wstring(p.offset) + L"(" + std::to_wstring(p.size) + L") ";
    }
    UE_LOGI("store_table_probe: params %ls frame=%d : %ls", label, R::FunctionFrameSize(fn),
            line.c_str());
}

// ---- shared: the row-name list via the reflected GetDataTableRowNames -------------------------
// A plain out-param native (NOT a CustomThunk), so a ProcessEvent call is well-defined:
// UObject::ProcessEvent builds the FOutParmRec chain for out params before invoking.
bool ReadRowNames(void* cdo, void* fn, void* table, std::vector<std::wstring>& out) {
    out.clear();
    if (!cdo || !fn || !table) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (!f.Set<void*>(L"Table", table)) return false;
    TArrayRaw arr{nullptr, 0, 0};
    if (!f.SetRaw(L"OutRowNames", &arr, sizeof(arr))) return false;
    if (!ue_wrap::Call(cdo, f)) return false;
    if (!f.GetRaw(L"OutRowNames", &arr, sizeof(arr))) return false;
    if (!arr.Data || arr.Num <= 0 || arr.Num > kSaneRowCap) {
        if (arr.Data) R::EngineFree(arr.Data);
        return false;
    }
    out.reserve(static_cast<size_t>(arr.Num));
    for (int32_t i = 0; i < arr.Num; ++i)
        out.push_back(R::ToString(*reinterpret_cast<R::FName*>(
            reinterpret_cast<uint8_t*>(arr.Data) + static_cast<size_t>(i) * sizeof(R::FName))));
    R::EngineFree(arr.Data);  // the engine allocated this array into our frame
    return true;
}

// ---- candidate (a): GetDataTableRowFromName ---------------------------------------------------
// Called with an OVERSIZED params blob and a canary, so that if the CustomThunk's compatibility
// check DOES pass (i.e. the reading this probe exists to test is wrong) its 0x4D-byte row memcpy
// lands in slack we own instead of corrupting anything. Whether it wrote is then a measurement.
void TryRowFromName(void* cdo, void* fn, void* table, const std::vector<std::wstring>& names,
                    int32_t priceOff) {
    if (!cdo || !fn) { Unavailable(L"(a) GetDataTableRowFromName", L"function not found"); return; }
    const int32_t offTable = R::FindParamOffset(fn, L"Table");
    const int32_t offName  = R::FindParamOffset(fn, L"RowName");
    const int32_t offRow   = R::FindParamOffset(fn, L"OutRow");
    const int32_t offRet   = R::FindParamOffset(fn, L"ReturnValue");
    if (offTable < 0 || offName < 0 || offRow < 0) {
        Unavailable(L"(a) GetDataTableRowFromName", L"param offsets unresolved");
        return;
    }
    if (priceOff < 0) { Unavailable(L"(a) GetDataTableRowFromName", L"price offset unresolved"); return; }

    // The params blob is deliberately far larger than the DECLARED frame: the frame is only
    // sizeof(Table)+sizeof(FName)+sizeof(FTableRowBase&)+bool, but if the CustomThunk's
    // compatibility check DOES pass it memcpys a whole 0x4D row at `offRow`, which would run past
    // a correctly-sized allocation. Oversizing means a wrong model costs a measurement, not a heap.
    // (An earlier revision also wrote a canary at +256 and never read it -- dead code: a 0x4D write
    // at offRow can never reach that far, so it could not catch anything. Removed.)
    const int32_t frameSize = R::FunctionFrameSize(fn);
    constexpr size_t kBlob = 4096;
    std::vector<uint8_t> parms(kBlob);

    std::vector<Row> rows;
    rows.reserve(names.size());
    int32_t wroteCount = 0;
    int32_t trueCount  = 0;
    for (const std::wstring& n : names) {
        std::fill(parms.begin(), parms.end(), static_cast<uint8_t>(0));
        *reinterpret_cast<void**>(parms.data() + offTable) = table;
        *reinterpret_cast<R::FName*>(parms.data() + offName) = ue_wrap::fname_utils::StringToFName(n);
        if (!R::CallFunction(cdo, fn, parms.data())) break;
        const bool ret = (offRet >= 0) && (parms[static_cast<size_t>(offRet)] != 0);
        if (ret) ++trueCount;
        // Did anything land in the out slot? (all-zero == untouched, since the blob was zeroed)
        bool wrote = false;
        for (size_t i = 0; i < 0x50 && (static_cast<size_t>(offRow) + i) < kBlob; ++i)
            if (parms[static_cast<size_t>(offRow) + i] != 0) { wrote = true; break; }
        if (wrote) ++wroteCount;
        Row r;
        r.name = LowerNarrow(n);
        if (wrote) {
            r.price = *reinterpret_cast<int32_t*>(parms.data() + offRow + priceOff);
            r.havePrice = true;
        }
        rows.push_back(std::move(r));
    }
    UE_LOGI("store_table_probe: (a) declaredFrame=%d returned-true=%d out-slot-written=%d of %zu",
            frameSize, trueCount, wroteCount, names.size());
    if (wroteCount == 0) {
        // State the OBSERVATION, not a cause. `returned-true=0` with the dispatch having happened
        // rules out "never called", but a rejected FTableRowBase compat check, a null
        // Stack.MostRecentProperty in the compiled-in path, and an OutParms walk that landed on a
        // different property all produce byte-identical output here. Naming one would be a
        // confirmation dressed as a measurement
        // ([[feedback-probe-must-count-not-confirm]]); separating them needs a debugger, and the
        // decision does not depend on which it is.
        Unavailable(L"(a) GetDataTableRowFromName",
                    L"dispatched for every row, returned false every time, never wrote the out "
                    L"slot -- unusable from C++; the cause among {compat check, MostRecentProperty, "
                    L"OutParms walk} is NOT determined by this instrument");
        return;
    }
    Verdict(L"(a) GetDataTableRowFromName", std::move(rows));
}

// ---- candidate (b): GetDataTableColumnAsString -------------------------------------------------
// Zero struct layout. Two legs under test: which NAME form matches the property, and whether the
// column order equals GetDataTableRowNames' order. The digests settle both at once.
bool TryColumnAsString(void* cdo, void* fn, void* table, const std::vector<std::wstring>& names,
                       const wchar_t* propName, const wchar_t* label) {
    if (!cdo || !fn) { Unavailable(label, L"function not found"); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (!f.Set<void*>(L"DataTable", table)) { Unavailable(label, L"param 'DataTable' unresolved"); return false; }
    if (!f.Set<R::FName>(L"PropertyName", ue_wrap::fname_utils::StringToFName(propName))) {
        Unavailable(label, L"param 'PropertyName' unresolved");
        return false;
    }
    if (!ue_wrap::Call(cdo, f)) { Unavailable(label, L"dispatch failed"); return false; }
    TArrayRaw arr{nullptr, 0, 0};
    if (!f.GetRaw(L"ReturnValue", &arr, sizeof(arr))) { Unavailable(label, L"no ReturnValue param"); return false; }
    if (!arr.Data || arr.Num <= 0 || arr.Num > kSaneRowCap) {
        if (arr.Data) R::EngineFree(arr.Data);
        Unavailable(label, L"empty column (property name did not match?)");
        return false;
    }

    std::vector<Row> rows;
    rows.reserve(static_cast<size_t>(arr.Num));
    for (int32_t i = 0; i < arr.Num; ++i) {
        auto* s = reinterpret_cast<FStringRaw*>(reinterpret_cast<uint8_t*>(arr.Data) +
                                                static_cast<size_t>(i) * sizeof(FStringRaw));
        Row r;
        r.name = (static_cast<size_t>(i) < names.size()) ? LowerNarrow(names[static_cast<size_t>(i)])
                                                         : std::string("<unnamed>");
        if (s->Data && s->Num > 0) {
            r.price = static_cast<int32_t>(_wtoi(s->Data));
            r.havePrice = true;
            R::EngineFree(s->Data);
        }
        rows.push_back(std::move(r));
    }
    R::EngineFree(arr.Data);
    if (static_cast<size_t>(arr.Num) != names.size())
        UE_LOGW("store_table_probe: %ls column len=%d != rowNames len=%zu -- the zip is unsound",
                label, arr.Num, names.size());
    Verdict(label, std::move(rows));
    return true;
}

// ---- candidate (c): raw UDataTable::RowMap walk ------------------------------------------------
// One derived constant (RowMap sits immediately after the reflected RowStruct) plus the
// TSetElement<TTuple<FName,uint8*>> stride. Both are caught by the digests.
void TryRowMapWalk(void* table, int32_t offRowStruct, int32_t priceOff) {
    if (offRowStruct < 0) { Unavailable(L"(c) RowMap walk", L"RowStruct property unresolved"); return; }
    if (priceOff < 0)     { Unavailable(L"(c) RowMap walk", L"price offset unresolved"); return; }
    constexpr int32_t kElemStride = 24;  // {FName key; uint8* row; int32 hashNext; int32 hashIndex}
    auto* rowMap = reinterpret_cast<uint8_t*>(table) + offRowStruct + sizeof(void*);
    const TArrayRaw elems = *reinterpret_cast<TArrayRaw*>(rowMap);
    if (!elems.Data || elems.Num <= 0 || elems.Num > kSaneRowCap) {
        Unavailable(L"(c) RowMap walk", L"element array head is not plausible");
        return;
    }
    std::vector<Row> rows;
    rows.reserve(static_cast<size_t>(elems.Num));
    for (int32_t i = 0; i < elems.Num; ++i) {
        auto* e = reinterpret_cast<uint8_t*>(elems.Data) + static_cast<size_t>(i) * kElemStride;
        const R::FName key = *reinterpret_cast<R::FName*>(e);
        auto* rowPtr = *reinterpret_cast<uint8_t**>(e + 8);
        Row r;
        r.name = LowerNarrow(R::ToString(key));
        if (rowPtr) {
            r.price = *reinterpret_cast<int32_t*>(rowPtr + priceOff);
            r.havePrice = true;
        }
        rows.push_back(std::move(r));
    }
    Verdict(L"(c) RowMap walk", std::move(rows));
}

}  // namespace

void Tick() {
    static const bool s_enabled =
        coop::config::ResolveFlag(::coop::config_registry::rows::store_table_probe);
    if (!s_enabled || g_done) return;

    void* table = R::FindObject(L"list_store", L"DataTable");
    if (!table) {
        // Retry next tick -- but say so ONCE. A probe that prints nothing when it cannot resolve is
        // indistinguishable from a probe that is switched off, and this one already cost a smoke
        // run to that ambiguity (the ini key had landed inside a comment, so an empty log read as
        // "ran, found nothing" instead of "never enabled").
        static int32_t s_waits = 0;
        if (++s_waits == 600)
            UE_LOGW("store_table_probe: ENABLED but `list_store` has not resolved after %d ticks "
                    "-- still retrying (this is not a result)", s_waits);
        return;
    }
    g_done = true;

    UE_LOGI("store_table_probe: === list_store row-reader census (expect rows=%d sum=%lld "
            "digest=%016llX names=%016llX) ===",
            kExpectRows, static_cast<long long>(kExpectPriceSum),
            static_cast<unsigned long long>(kExpectDigest),
            static_cast<unsigned long long>(kExpectNameDigest));

    // The row struct + its `price` member, both by NAME. Candidates (a) and (c) read a raw row
    // through this offset; candidate (b) needs no layout at all.
    void* dtCls = R::ClassOf(table);
    const int32_t offRowStruct = dtCls ? R::FindPropertyOffset(dtCls, L"RowStruct") : -1;
    void* rowStruct = (offRowStruct >= 0)
                          ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(table) + offRowStruct)
                          : nullptr;
    const int32_t priceOff = rowStruct ? R::FindPropertyOffsetByPrefix(rowStruct, L"price_") : -1;
    UE_LOGI("store_table_probe: table=%p class=%ls RowStruct@%d -> %p (%ls) price@%d",
            table, R::ClassNameOf(table).c_str(), offRowStruct, rowStruct,
            rowStruct ? R::ClassNameOf(rowStruct).c_str() : L"-", priceOff);

    // The mangled member name is what a BP UserDefinedStruct actually calls its field; candidate
    // (b) is tried with BOTH that and the friendly form, because which one the engine matches is
    // exactly one of its unmeasured legs.
    std::wstring mangledPrice;
    if (rowStruct) {
        for (const auto& fld : R::EnumerateStructFields(rowStruct)) {
            if (fld.name.rfind(L"price_", 0) == 0) { mangledPrice = fld.name; break; }
        }
    }
    UE_LOGI("store_table_probe: row struct fields: mangled price name = '%ls'",
            mangledPrice.empty() ? L"<none>" : mangledPrice.c_str());

    void* cdo = R::FindClassDefaultObject(L"DataTableFunctionLibrary");
    void* cls = cdo ? R::ClassOf(cdo) : nullptr;
    void* fnNames = cls ? R::FindFunction(cls, L"GetDataTableRowNames") : nullptr;
    void* fnCol   = cls ? R::FindFunction(cls, L"GetDataTableColumnAsString") : nullptr;
    void* fnRow   = cls ? R::FindFunction(cls, L"GetDataTableRowFromName") : nullptr;
    LogParams(L"GetDataTableRowNames", fnNames);
    LogParams(L"GetDataTableColumnAsString", fnCol);
    LogParams(L"GetDataTableRowFromName", fnRow);

    std::vector<std::wstring> names;
    if (!ReadRowNames(cdo, fnNames, table, names)) {
        UE_LOGW("store_table_probe: GetDataTableRowNames failed -- (a) and (b) cannot be scored; "
                "running (c) alone");
        TryRowMapWalk(table, offRowStruct, priceOff);
        return;
    }
    UE_LOGI("store_table_probe: GetDataTableRowNames -> %zu names", names.size());

    TryRowFromName(cdo, fnRow, table, names, priceOff);
    if (!mangledPrice.empty())
        TryColumnAsString(cdo, fnCol, table, names, mangledPrice.c_str(),
                          L"(b) ColumnAsString[mangled]");
    TryColumnAsString(cdo, fnCol, table, names, L"price", L"(b) ColumnAsString[friendly]");
    TryRowMapWalk(table, offRowStruct, priceOff);

    UE_LOGI("store_table_probe: === census done (one-shot) ===");
}

}  // namespace coop::dev::store_table_probe
