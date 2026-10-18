// coop/dev/hand_drop_census.h -- the hand-drop selftest's world census: the live keyed props within a radius of a
// centre, and the difference of two such lists by key. A private header of coop/dev/hand_drop_selftest.cpp and
// coop/dev/hand_drop_census.cpp (src tree, not include/).

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"  // FVector

#include <string>
#include <vector>

namespace coop::dev::hand_drop_census {

struct PropRow {
    void*        actor = nullptr;
    std::wstring key;
    std::wstring cls;
    float        distCm = 0.f;
};

// Keyed props whose location could not be read, each latched once; the selftest's centre reads share it and clear it at a
// whole-run end.
extern std::vector<ue_wrap::CachedObjRef> g_unreadable;

// Every live keyed prop within `radiusCm` of `centre`, sorted by key; `unread` takes those whose location could not be
// read, anywhere. Game thread.
std::vector<PropRow> Census(const ue_wrap::FVector& centre, float radiusCm, std::vector<PropRow>& unread);

// What `now` has that the baseline keys did not, and what the baseline had that `now` lost, as keys.
std::wstring DescribeDiff(const std::vector<std::wstring>& baseline, const std::vector<PropRow>& now);

}  // namespace coop::dev::hand_drop_census
