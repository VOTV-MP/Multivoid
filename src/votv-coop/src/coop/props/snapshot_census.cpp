// coop/snapshot_census.cpp -- see header.

#include "coop/props/snapshot_census.h"

#include "ue_wrap/prop.h"         // IsChipPile
#include "ue_wrap/reflection.h"   // NumObjects / ObjectAt / IsLive / ClassNameOf
#include "ue_wrap/log.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace coop::snapshot_census {

namespace R = ue_wrap::reflection;

namespace {

// CLIENT-side: host live count per chipPile class, for the bracket in progress.
std::unordered_map<std::wstring, uint32_t> g_hostCensus;
bool g_have = false;

}  // namespace

int BuildHostTail(std::vector<uint8_t>& outTail, int budgetBytes) {
    // INDEPENDENT of the expression path: a raw GUObjectArray walk sees every live actor whether or
    // not it is in the Prop Element registry. The 11:16 root was untracked piles MISSING from the
    // registry -> never expressed; they are still counted here. IsChipPile is a cheap class-hierarchy
    // test (no per-object string alloc); ClassNameOf runs only for the actual chipPiles.
    std::unordered_map<std::wstring, uint32_t> byClass;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (!ue_wrap::prop::IsChipPile(o)) continue;
        ++byClass[R::ClassNameOf(o)];
    }

    // Order by count DESC so the MASS classes (the only ones a wipe is catastrophic for) always
    // make the byte budget; only tiny classes can be omitted (and the >50% valve still covers them).
    std::vector<std::pair<std::wstring, uint32_t>> ranked(byClass.begin(), byClass.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& l, const auto& r) { return l.second > r.second; });

    outTail.clear();
    outTail.resize(2);  // reserve the uint16 classCount header
    int emitted = 0, omitted = 0;
    uint32_t omittedTopCount = 0;
    std::wstring omittedTopCls;
    for (const auto& [cls, cnt] : ranked) {
        const uint16_t nameLen = static_cast<uint16_t>(cls.size());
        const int entryBytes = 2 + static_cast<int>(nameLen) * 2 + 4;
        if (static_cast<int>(outTail.size()) + entryBytes > budgetBytes) {
            ++omitted;
            if (cnt > omittedTopCount) { omittedTopCount = cnt; omittedTopCls = cls; }
            continue;
        }
        size_t off = outTail.size();
        outTail.resize(off + static_cast<size_t>(entryBytes));
        std::memcpy(&outTail[off], &nameLen, 2);                        off += 2;
        std::memcpy(&outTail[off], cls.data(), static_cast<size_t>(nameLen) * 2); off += static_cast<size_t>(nameLen) * 2;
        std::memcpy(&outTail[off], &cnt, 4);
        ++emitted;
    }
    const uint16_t cc = static_cast<uint16_t>(emitted);
    std::memcpy(&outTail[0], &cc, 2);

    if (omitted > 0) {
        UE_LOGW("snapshot_census: emitted %d class(es), OMITTED %d small class(es) over the %d-byte "
                "budget (top omitted: %u x '%ls') -- the >50%% valve still backstops the omitted tail",
                emitted, omitted, budgetBytes, omittedTopCount, omittedTopCls.c_str());
    }
    UE_LOGI("snapshot_census: host completeness census built -- %d chipPile class(es) total, %d emitted "
            "on the wire (top: %u x '%ls')",
            static_cast<int>(ranked.size()), emitted,
            ranked.empty() ? 0u : ranked.front().second,
            ranked.empty() ? L"(none)" : ranked.front().first.c_str());
    return emitted;
}

void SetFromWire(const uint8_t* tail, size_t len) {
    g_hostCensus.clear();
    g_have = false;
    if (!tail || len < 2) return;
    size_t off = 0;
    uint16_t cc = 0;
    std::memcpy(&cc, tail + off, 2);
    off += 2;
    for (uint16_t i = 0; i < cc; ++i) {
        if (off + 2 > len) { UE_LOGW("snapshot_census: truncated tail (name len) -- ignoring rest"); break; }
        uint16_t nameLen = 0;
        std::memcpy(&nameLen, tail + off, 2);
        off += 2;
        if (off + static_cast<size_t>(nameLen) * 2 + 4 > len) {
            UE_LOGW("snapshot_census: truncated tail (entry body) -- ignoring rest");
            break;
        }
        std::wstring cls;
        cls.resize(nameLen);
        if (nameLen > 0) std::memcpy(&cls[0], tail + off, static_cast<size_t>(nameLen) * 2);
        off += static_cast<size_t>(nameLen) * 2;
        uint32_t cnt = 0;
        std::memcpy(&cnt, tail + off, 4);
        off += 4;
        g_hostCensus[cls] = cnt;
    }
    g_have = !g_hostCensus.empty();
    if (g_have) {
        uint32_t top = 0;
        std::wstring topCls;
        for (const auto& [c, v] : g_hostCensus) if (v > top) { top = v; topCls = c; }
        UE_LOGI("snapshot_census: client received host completeness census -- %zu class(es) (top: %u x '%ls')",
                g_hostCensus.size(), top, topCls.c_str());
    }
}

void Reset() {
    g_hostCensus.clear();
    g_have = false;
}

int HostCountForClass(const std::wstring& cls) {
    auto it = g_hostCensus.find(cls);
    return (it == g_hostCensus.end()) ? -1 : static_cast<int>(it->second);
}

bool HasCensus() { return g_have; }

}  // namespace coop::snapshot_census
