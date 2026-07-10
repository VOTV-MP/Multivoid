// coop/props/prop_element_tracker_detail.h -- INTERNAL shared state between
// prop_element_tracker.cpp (Mark/Unmark maintenance + reaper),
// prop_census.cpp (the seed/re-seed GUObjectArray walk, extracted 2026-07-10
// when the tracker passed the 800-LOC soft cap), and prop_key_index.cpp (the
// key -> live-actor index family, extracted 2026-07-10, second slice).
//
// Sibling-internal header (the net/session_lanes.h / event_dispatch.h
// precedent) -- NOT part of the public coop/ include surface; only the three
// TUs above include it.
//
// The maintained set semantics are the tracker's (see the H2-redux comment
// there): seeded once by the census walk, then maintained by Init POST
// (insert) + K2_DestroyActor PRE (evict). Mutex held only for brief set ops;
// no engine calls under lock.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

namespace coop::prop_element_tracker {

extern std::mutex g_knownKeyedPropsMutex;
extern std::unordered_set<void*> g_knownKeyedProps;
inline constexpr size_t kKnownKeyedPropsCap = 16384;

// Key-index private helpers (defined in prop_key_index.cpp, which solely owns
// the g_keyToActor / g_actorToKey maps + their leaf mutex). The tracker's
// MarkPropElement / UnmarkKnownKeyedProp / ReapDeadLocalPropElements call these
// two; every other index access goes through the public header's lookups.
// Caller must hold NO other mutex (the key-index mutex is a LEAF).
void IndexKeyForActor_(void* actor, const std::wstring& key, int32_t internalIdx);
void EraseKeyIndexForActor_(void* actor);

}  // namespace coop::prop_element_tracker
