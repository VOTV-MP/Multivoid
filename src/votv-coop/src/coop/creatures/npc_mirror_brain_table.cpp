// coop/creatures/npc_mirror_brain_table.cpp -- the entries of each parked class's event graph that run on a marked
// mirror (coop/creatures/npc_mirror_brain.h). An entry is a cooked byte offset into the class's ubergraph, so a game
// update moves it; every other entry of the graph is refused, and an entry the loaded graph lacks is refused with
// them. Each row says what the entry does and why it is presentation only.

#include "coop/creatures/npc_mirror_brain.h"

#include "ue_wrap/actors/fossilhound.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstddef>
#include <cstdint>

namespace coop::npc_mirror_brain {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// npc_zombie_C
constexpr int32_t kZombie[] = {
    // step: the walk's footstep notify reaches it through the animation-events interface. It calls the library's
    // step (a footstep sound and emitter) and the zombie's own empty `stepped`; it spawns nothing.
    19490,
};

// npc_zombie_alienSummoner_C, npc_zombie_corpse_C and npc_zombie_skerfuro_C own a graph of their own and allow none
// of it: its entries are the landing, the destroy, the corpse's `died`, the summon timer and their resumes, and each
// spawns actors, starts a timer or moves the zombie (skerfuro's resume 30 starts a chain that ends in `doWalk`).

// fossilhound_C
constexpr int32_t kFossilhound[] = {
    // step: the footstep notify. A trace, a physical-material sound, a sound, and every fourth step the voice.
    8427,
    // the per-viewer loop: the camera-distance test and its event count, re-entering itself on a 0.25 s delay. Our
    // setup starts it (ue_wrap::fossilhound), and each client counts its own view, as the class does.
    ue_wrap::fossilhound::kPerViewerEntry,
    // ReceiveDestroyed: the `false` of the loop's `true` when the hound was in view, so the count comes back.
    10636,
};

constexpr EntryTable kTables[] = {
    {P::name::NpcClass_Zombie, kZombie, sizeof(kZombie) / sizeof(kZombie[0])},
    {L"npc_zombie_alienSummoner_C", nullptr, 0},
    {L"npc_zombie_corpse_C", nullptr, 0},
    {L"npc_zombie_skerfuro_C", nullptr, 0},
    {P::name::NpcClass_Fossilhound, kFossilhound, sizeof(kFossilhound) / sizeof(kFossilhound[0])},
};

}  // namespace

const EntryTable* TableFor(void* graphClass) {
    if (!graphClass) return nullptr;
    const R::FName& name = R::NameOf(graphClass);
    for (const EntryTable& t : kTables)
        if (R::NameEquals(name, t.className)) return &t;
    return nullptr;
}

}  // namespace coop::npc_mirror_brain
