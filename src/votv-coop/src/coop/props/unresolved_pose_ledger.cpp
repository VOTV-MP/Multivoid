// coop/props/unresolved_pose_ledger.cpp -- see the header for the field measurement this
// implements and why the thresholds are where they are.

#include "coop/props/unresolved_pose_ledger.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <unordered_map>

namespace coop::unresolved_pose_ledger {

namespace {

// Bounded by construction: a session that somehow accumulates this many distinct unresolved
// identities has a different problem, and an unbounded map on a receive path is its own bug.
constexpr size_t kLedgerCap = 256;

struct Entry {
    uint32_t count    = 0;
    uint64_t firstMs  = 0;
    bool     reported = false;   // latched: the sustained WARN fires at most once per identity
};

std::unordered_map<std::wstring, Entry> g_rows;

// A pose with no key (the eid-only clump stream) still needs a distinct row, or every keyless
// miss from one slot would collapse into a single entry and the count would mean nothing.
std::wstring RowKey(int slot, const std::wstring& keyW, uint32_t eid) {
    std::wstring k = std::to_wstring(slot);
    k += L'|';
    if (keyW.empty() || keyW == L"None") {
        k += L'#';
        k += std::to_wstring(eid);
    } else {
        k += keyW;
    }
    return k;
}

}  // namespace

bool Note(int slot, const std::wstring& keyW, uint32_t eid, uint64_t nowMs) {
    UE_ASSERT_GAME_THREAD("unresolved_pose_ledger (Note)");
    if (g_rows.size() >= kLedgerCap) {
        UE_LOGW("unresolved_pose_ledger: %zu distinct unresolved identities -- clearing. That is "
                "far past anything the field has shown; suspect a systemic identity break rather "
                "than a race.", g_rows.size());
        g_rows.clear();
    }
    Entry& e = g_rows[RowKey(slot, keyW, eid)];
    if (e.count == 0) e.firstMs = nowMs;
    ++e.count;
    if (e.reported) return false;
    if (e.count >= kSustainedCount && (nowMs - e.firstMs) >= kSustainedMs) {
        e.reported = true;
        return true;
    }
    return false;
}

void Clear(int slot, const std::wstring& keyW, uint32_t eid) {
    UE_ASSERT_GAME_THREAD("unresolved_pose_ledger (Clear)");
    if (g_rows.empty()) return;
    const auto it = g_rows.find(RowKey(slot, keyW, eid));
    if (it == g_rows.end()) return;
    if (it->second.reported) {
        UE_LOGI("unresolved_pose_ledger: slot %d key '%ls' eid=%u RESOLVED after %u unresolved "
                "pose(s) -- the gap closed on its own",
                slot, keyW.c_str(), eid, it->second.count);
    }
    g_rows.erase(it);
}

size_t ResetSlot(int slot) {
    UE_ASSERT_GAME_THREAD("unresolved_pose_ledger (ResetSlot)");
    const std::wstring prefix = std::to_wstring(slot) + L"|";
    size_t dropped = 0;
    for (auto it = g_rows.begin(); it != g_rows.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = g_rows.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

}  // namespace coop::unresolved_pose_ledger
