// ue_wrap/world/physsound_table.cpp -- see ue_wrap/world/physsound_table.h.

#include "ue_wrap/world/physsound_table.h"

#include "ue_wrap/core/object_path.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/data_table.h"

#include <cwctype>
#include <set>

namespace ue_wrap::physsound_table {
namespace {

namespace R  = ue_wrap::reflection;
namespace DT = ue_wrap::data_table;

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

}  // namespace

Read ReadImpactSounds(std::vector<std::wstring>& out, std::string& why) {
    out.clear();
    void* const table = R::FindObject(L"list_physSound", L"DataTable");
    if (!table) { why = "list_physSound is not loaded"; return Read::NotYet; }
    void* const rowStruct = DT::RowStruct(table);
    if (!rowStruct) { why = "list_physSound's RowStruct does not resolve"; return Read::NotYet; }
    const int32_t off = R::FindPropertyOffsetByPrefix(rowStruct, L"impact_");
    if (off < 0) { why = "the row struct has no impact member"; return Read::Mismatch; }

    std::vector<DT::RowRef> refs;
    if (!DT::Rows(table, refs, "physsound_table")) { why = "the RowMap walk is not plausible"; return Read::NotYet; }

    // The gate: the engine's own read of the same column. A null cell reads "None" (or nothing), a set one names the
    // object, in whatever form the engine writes an object reference.
    std::vector<std::wstring> column;
    const std::wstring mangled = DT::MemberName(rowStruct, L"impact_");
    if ((mangled.empty() || !DT::ColumnAsStrings(table, mangled, column)) && !DT::ColumnAsStrings(table, L"impact", column)) {
        why = "the impact column could not be read, so the walk cannot be verified";
        return Read::NotYet;
    }
    if (column.size() != refs.size()) {
        why = "gate length mismatch: walk " + std::to_string(refs.size()) + ", column " + std::to_string(column.size());
        return Read::Mismatch;
    }
    std::set<std::wstring> distinct;
    for (size_t i = 0; i < refs.size(); ++i) {
        void* const cue = *reinterpret_cast<void* const*>(refs[i].row + off);
        const std::wstring text = Lower(column[i]);
        if (!cue) {
            if (!text.empty() && text != L"none") {
                why = "gate disagrees at row " + std::to_string(i) + ": the walk reads no sound, the column reads '" +
                      Narrow(column[i]) + "'";
                return Read::Mismatch;
            }
            continue;
        }
        const std::wstring name = Lower(R::ToString(R::NameOf(cue)));
        if (text.find(name) == std::wstring::npos) {
            why = "gate disagrees at row " + std::to_string(i) + ": the walk reads '" + Narrow(name) + "', the column reads '" +
                  Narrow(column[i]) + "'";
            return Read::Mismatch;
        }
        distinct.insert(ue_wrap::object_path::Of(cue));
    }
    out.assign(distinct.begin(), distinct.end());
    return Read::Ok;
}

}  // namespace ue_wrap::physsound_table
