// coop/prop_echo_suppress.cpp -- see header for design.

#include "coop/props/prop_echo_suppress.h"

#include <unordered_set>

namespace coop::prop_echo_suppress {
namespace {

// Game-thread-only access (OnSpawn/OnDestroy in remote_prop and the
// Init POST / K2_DestroyActor PRE observers in prop_lifecycle all
// dispatch on the game thread). Capped to bound memory across long
// sessions; on overflow we clear (a one-shot stale lookup is harmless
// -- see header).
std::unordered_set<void*> g_incomingSpawns;
std::unordered_set<void*> g_incomingDestroys;
constexpr size_t kIncomingCap = 256;

template <class Set>
void InsertCapped(Set& s, void* actor) {
    if (s.size() >= kIncomingCap) s.clear();
    s.insert(actor);
}

template <class Set>
bool TakeOne(Set& s, void* actor) {
    auto it = s.find(actor);
    if (it == s.end()) return false;
    s.erase(it);
    return true;
}

}  // namespace

void MarkIncomingSpawn(void* actor)     { if (actor) InsertCapped(g_incomingSpawns, actor); }
bool ConsumeIncomingSpawn(void* actor)  { return actor ? TakeOne(g_incomingSpawns, actor) : false; }
void MarkIncomingDestroy(void* actor)   { if (actor) InsertCapped(g_incomingDestroys, actor); }
bool ConsumeIncomingDestroy(void* actor){ return actor ? TakeOne(g_incomingDestroys, actor) : false; }

}  // namespace coop::prop_echo_suppress
