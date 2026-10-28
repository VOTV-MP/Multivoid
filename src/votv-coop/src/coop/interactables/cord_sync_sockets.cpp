// coop/interactables/cord_sync_sockets.cpp -- see coop/interactables/cord_sync.h. The level sockets by their keys: a
// fixed set per world, read from the object index once per world generation, so a peer's key is only ever a lookup in
// this table and never a name the engine's pool would keep.

#include "coop/interactables/cord_sync.h"

#include "cord_sync_detail.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/devices/cord.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::cord_sync {
namespace {

namespace CD = ue_wrap::cord;
namespace OI = ue_wrap::object_index;
namespace WI = ue_wrap::world_identity;

// More sockets than this in a world is a level this lane has not been measured on (61 were).
constexpr int32_t kMaxSockets = 512;

struct Table {
    std::unordered_map<std::wstring, void*> byKey;
    uint32_t world = 0;      // the world generation of the last build; 0 is none built
    uint64_t classSet = 0;   // the object index's class set at the last build
};
Table g_table;

// Reads the world's cordSocket_C instances. A child socket (the form 2 link, named by its owner prop) is not in the
// table, nor is a socket whose key the wire cannot carry.
void Build() {
    ++detail::Stat().sockBuilds;
    g_table.byKey.clear();
    g_table.world = WI::Generation();
    g_table.classSet = OI::ClassSetVersion();
    void* found[kMaxSockets];
    const int32_t n = CD::Sockets(found, kMaxSockets);
    static bool sCapSaid = false;
    if (n >= kMaxSockets && !sCapSaid) {
        sCapSaid = true;
        UE_LOGW("cord_sync: %d cord sockets in this world fill the table -- later ones are not found (said once)", n);
    }
    for (int32_t i = 0; i < n; ++i) {
        std::wstring owner, child;
        if (CD::ChildSocketName(found[i], owner, child)) continue;
        const std::wstring key = ue_wrap::prop::GetActorSaveKeyString(found[i]);
        if (key.empty() || key == L"None" || key.size() > detail::kKeyMax) continue;
        g_table.byKey.emplace(key, found[i]);
    }
}

}  // namespace

// Built at the first lookup of a world generation. A miss rebuilds it once, and only when the object index's class set
// has moved since the last build (a class loaded or unloaded) or the world has: a key no socket holds is answered from
// the table, at the cost of one hash.
void* LevelSocket(const std::wstring& key) {
    if (key.empty() || !CD::EnsureResolved()) return nullptr;
    bool fresh = false;
    if (g_table.world != WI::Generation()) {
        Build();
        fresh = true;
    }
    for (;;) {
        const auto it = g_table.byKey.find(key);
        if (it != g_table.byKey.end() && CD::IsSocket(it->second)) return it->second;
        if (fresh || g_table.classSet == OI::ClassSetVersion()) return nullptr;
        Build();
        fresh = true;
    }
}

namespace detail {

void ClearSockets() { g_table = Table{}; }

}  // namespace detail
}  // namespace coop::cord_sync
