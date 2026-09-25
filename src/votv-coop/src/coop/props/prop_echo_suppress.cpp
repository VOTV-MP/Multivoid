// coop/props/prop_echo_suppress.cpp -- see header for design.

#include "coop/props/prop_echo_suppress.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/reflection.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace coop::prop_echo_suppress {
namespace {

namespace R = ue_wrap::reflection;

// A slot's serial at the mark, by slot. A later mark of the same slot, its successor's, replaces the
// entry; the entries whose slot no longer holds that serial are dead and are pruned whenever the set
// has doubled since the last prune, so a set of N live marks costs O(1) per mark over time.
constexpr size_t kPruneFloor = 256;
struct MarkSet {
    std::unordered_map<int32_t, int32_t> serialBySlot;
    size_t pruneAt = kPruneFloor;
};
MarkSet g_mirrorSpawns;
MarkSet g_incomingDestroys;

void Prune(MarkSet& m) {
    for (auto it = m.serialBySlot.begin(); it != m.serialBySlot.end();)
        it = R::SlotSerial(it->first) == it->second ? std::next(it) : m.serialBySlot.erase(it);
    m.pruneAt = std::max(kPruneFloor, 2 * m.serialBySlot.size());
}

void Mark(MarkSet& m, void* actor) {
    if (!actor) return;
    const int32_t slot = R::InternalIndexOf(actor);
    const int32_t serial = R::AllocateSlotSerial(slot);
    if (serial == 0) return;  // not an object the array maps
    if (m.serialBySlot.size() >= m.pruneAt) Prune(m);
    m.serialBySlot[slot] = serial;
}

bool Has(const MarkSet& m, void* actor) {
    if (!actor) return false;
    const int32_t slot = R::InternalIndexOf(actor);
    const auto it = m.serialBySlot.find(slot);
    return it != m.serialBySlot.end() && R::ObjectAt(slot) == actor && R::SlotSerial(slot) == it->second;
}

// Mirror-spawn re-entrancy depth (see header). Game-thread-only; a plain int because the wrapped
// BeginDeferred call dispatches synchronously on the same thread (nested scopes are fine -- depth
// counts).
int g_mirrorSpawnDepth = 0;

}  // namespace

void MarkMirrorSpawn(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_echo_suppress::MarkMirrorSpawn");
    Mark(g_mirrorSpawns, actor);
}

bool IsMirrorSpawn(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_echo_suppress::IsMirrorSpawn");
    return Has(g_mirrorSpawns, actor);
}

void MarkIncomingDestroy(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_echo_suppress::MarkIncomingDestroy");
    Mark(g_incomingDestroys, actor);
}

bool IsIncomingDestroy(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_echo_suppress::IsIncomingDestroy");
    return Has(g_incomingDestroys, actor);
}

// ---- the ARBITER-CONSUMED key set ---------------------------------------------------------------
// Keyed by save KEY, not by object, because the whole point is that the actor is already gone: the
// sets above cannot express "a destroy naming THIS KEY is an echo of one I performed myself".
constexpr size_t kArbiterKeyCap = 256;
std::unordered_set<std::wstring> g_arbiterConsumedKeys;

void MarkArbiterConsumedKey(const std::wstring& key) {
    if (key.empty()) return;
    if (g_arbiterConsumedKeys.size() >= kArbiterKeyCap) g_arbiterConsumedKeys.clear();
    g_arbiterConsumedKeys.insert(key);
}

bool ConsumeArbiterConsumedKey(const std::wstring& key) {
    if (key.empty()) return false;
    auto it = g_arbiterConsumedKeys.find(key);
    if (it == g_arbiterConsumedKeys.end()) return false;
    g_arbiterConsumedKeys.erase(it);
    return true;
}

ScopedMirrorSpawn::ScopedMirrorSpawn()  { ++g_mirrorSpawnDepth; }
ScopedMirrorSpawn::~ScopedMirrorSpawn() { --g_mirrorSpawnDepth; }
bool InMirrorSpawnScope()               { return g_mirrorSpawnDepth > 0; }

}  // namespace coop::prop_echo_suppress
