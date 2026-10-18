// coop/dev/hand_drop_census.cpp -- the hand-drop selftest's world census, see coop/dev/hand_drop_census.h.

#include "hand_drop_census.h"  // co-located private header (src tree, not include/)

#include "coop/player/hand_item.h"        // the hand axis a census must not count
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::hand_drop_census {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;

// Every live keyed prop within `radiusCm` of `centre`, sorted by key so both peers print the same
// list in the same order. The hand axis is excluded on both roles: a peer's display mirror is a
// real actor of the held item's class sitting at that peer's hands, so counting it would hide
// exactly the loss this driver measures -- the census would read "still one prop here" while the
// only thing there is the mirror that is about to be destroyed.
// Keyed props whose location could not be read: each read once, since a failed read of a live actor faults
// again, and reported by every census while it lives.
std::vector<ue_wrap::CachedObjRef> g_unreadable;

// `unread`: keyed props whose location could not be read -- anywhere, since an unread prop has no distance.
std::vector<PropRow> Census(const ue_wrap::FVector& centre, float radiusCm, std::vector<PropRow>& unread) {
    std::vector<PropRow> out;
    void* handAxis[1 + coop::players::kMaxPeers];
    const size_t handAxisN =
        coop::hand_item::CollectHandAxisActors(handAxis, 1 + coop::players::kMaxPeers);
    const float r2 = radiusCm * radiusCm;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!PR::IsDescendantOfProp(obj)) continue;
        if (!R::IsLive(obj)) continue;
        if (E::IsChildActor(obj)) continue;
        bool isHandAxis = false;
        for (size_t h = 0; h < handAxisN; ++h) if (handAxis[h] == obj) { isHandAxis = true; break; }
        if (isHandAxis) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        std::wstring key = PR::GetKeyString(obj);
        if (key.empty() || key == L"None") continue;  // keyless props ride other lanes
        bool latched = false;
        for (const ue_wrap::CachedObjRef& u : g_unreadable)
            if (u.Is(obj)) { latched = true; break; }
        ue_wrap::FVector loc{};
        if (latched || !E::TryGetActorLocation(obj, loc)) {
            if (!latched) {
                g_unreadable.emplace_back();
                g_unreadable.back().Set(obj);
            }
            PropRow row;
            row.actor = obj;
            row.key   = std::move(key);
            row.cls   = R::ClassNameOf(obj);
            unread.push_back(std::move(row));
            continue;
        }
        const float dx = loc.X - centre.X, dy = loc.Y - centre.Y, dz = loc.Z - centre.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) continue;
        PropRow row;
        row.actor  = obj;
        row.key    = std::move(key);
        row.cls    = R::ClassNameOf(obj);
        row.distCm = std::sqrt(d2);
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(),
              [](const PropRow& a, const PropRow& b) { return a.key < b.key; });
    return out;
}

// What this census has that the baseline did not, and what the baseline had that it lost. Both
// lists are keys, so the two peers' logs name the same props.
std::wstring DescribeDiff(const std::vector<std::wstring>& baseline,
                          const std::vector<PropRow>& now) {
    auto has = [](const std::vector<std::wstring>& v, const std::wstring& k) {
        return std::find(v.begin(), v.end(), k) != v.end();
    };
    std::vector<std::wstring> nowKeys;
    nowKeys.reserve(now.size());
    for (const PropRow& r : now) nowKeys.push_back(r.key);

    std::wstring added, gone;
    for (const PropRow& r : now)
        if (!has(baseline, r.key)) { if (!added.empty()) added += L", "; added += L"'" + r.key + L"' (" + r.cls + L")"; }
    for (const std::wstring& k : baseline)
        if (!has(nowKeys, k)) { if (!gone.empty()) gone += L", "; gone += L"'" + k + L"'"; }
    std::wstring s = L"gained[" + (added.empty() ? std::wstring(L"none") : added) + L"] lost[" +
                     (gone.empty() ? std::wstring(L"none") : gone) + L"]";
    return s;
}

}  // namespace coop::dev::hand_drop_census
