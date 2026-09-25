// ue_wrap/world/event_list.cpp -- see ue_wrap/world/event_list.h.

#include "ue_wrap/world/event_list.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/data_table.h"

#include <vector>

namespace ue_wrap::event_list {
namespace {

namespace R  = reflection;
namespace DT = data_table;

// The answer, read once the table has loaded: it never changes after.
bool    g_ok = false;
bool    g_structural = false;  // the table is there, but its rows or member cannot be read: for the process
bool    g_saidAbsent = false;
int32_t g_lastDay = -1;

// The last row's day. The rows come in the RowMap's own order, the order GetDataTableRowNames hands the
// rollover, whose last name is the row it reads. A table not loaded yet is not a verdict; its absence
// costs one walk of the object array a call, and the one caller asks once a midnight.
bool ReadOnce() {
    void* table = R::FindObject(L"list_events", L"DataTable");
    if (!table) {
        if (!g_saidAbsent) UE_LOGW("event_list: list_events is not loaded -- asked again at the next call");
        g_saidAbsent = true;
        return false;
    }
    void* rowStruct = DT::RowStruct(table);
    const int32_t timeOff = rowStruct ? R::FindPropertyOffsetByPrefix(rowStruct, L"time_") : -1;
    std::vector<DT::RowRef> rows;
    if (timeOff < 0 || !DT::Rows(table, rows, "event_list") || rows.empty()) {
        g_structural = true;
        UE_LOGW("event_list: list_events' last day did not resolve (time@%d, %zu rows) -- the story's end is out of "
                "reach", timeOff, rows.size());
        return false;
    }
    // The time is an FIntVector: X, Y, then Z, the day.
    g_lastDay = *reinterpret_cast<const int32_t*>(rows.back().row + timeOff + 2 * sizeof(int32_t));
    UE_LOGI("event_list: list_events holds %zu rows; the last, '%ls', runs on day %d", rows.size(),
            R::ToString(rows.back().key).c_str(), g_lastDay);
    return true;
}

}  // namespace

bool ReadLastDay(int32_t& out) {
    if (!g_ok && !g_structural) g_ok = ReadOnce();
    if (!g_ok) return false;
    out = g_lastDay;
    return true;
}

}  // namespace ue_wrap::event_list
