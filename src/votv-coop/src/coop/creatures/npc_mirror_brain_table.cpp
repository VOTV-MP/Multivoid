// coop/creatures/npc_mirror_brain_table.cpp -- the entries of each parked class's event graph that run on a marked
// mirror (coop/creatures/npc_mirror_brain.h). An entry is a cooked byte offset into the class's ubergraph, so a game
// update moves it; every other entry of the graph is refused, and an entry the loaded graph lacks is refused with
// them. Each row says what the entry does and why it is presentation only.

#include "coop/creatures/npc_mirror_brain.h"

#include "ue_wrap/actors/fossilhound.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstddef>
#include <cstdint>
#include <cwchar>

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

// geomOcta_C
//
// Only the class's DECISIONS are refused; its BeginPlay, tick, timelines, montages and sounds run on the mirror, and
// coop/creatures/geom_octa_sync relays the host's decisions into it. The twelve entries refused, with what each does:
//   5538 move (the wander: a random point on this machine's navmesh) and 466 its Delay(1) resume;
//   2783 and 2659 the move-to proxy's success and fail, and 481 their 1-5 s wait's resume (the next move);
//   3766 minSphere's begin-overlap and 6252 propAggroSphere's (the target, rolled against this machine's pawns);
//   3592 aggro (the target write, the montage start);
//   6806 despawnAttempt (the leave decision) and 573 its Delay(60) resume (the destroy);
//   496 BeginPlay's Delay(300) resume (it only arms the despawn timer);
//   1092 the "fire" montage's finish notify (re-fire or close, from this machine's overlap).
// The twenty-five allowed are below, each presentation or the deterministic continuation of a relayed decision. As
// MTA's remote ped runs on relayed sync state while the game animates it, the Octa's output lock is the explosion
// lane's dissolve of its laser's blast (the header's precedent paragraph cites MTA).
constexpr int32_t kGeomOcta[] = {
    // BeginPlay: the 3 s hum timer and the mesh attached to the spring arm. Its first move() and its Delay(300) land in
    // refused entries.
    5940,
    // ReceiveTick: the mesh turns toward the target's direction, and while charging the ring emitter and charge sound
    // spawn at the muzzle. `despawn` stays false on a mirror, so the climb branch never runs.
    4007,
    // sound: with no target, the hum at the actor.
    6082,
    // rotateTo, and the rotation timeline's update (rotA) and finish: the turn toward the target and back.
    5380, 5935, 6223,
    // charge: the beam loop, isCharge and the charge timeline played or reversed.
    5399,
    // The charge timeline's update (glow power and scale, the beam loop's volume and pitch) and its finish, which
    // ends the charge and calls fire().
    2906, 3761,
    // fire(): lastRot, isAnim, the "fire" montage section, charge(false) and the laser's deferred spawn. The laser's
    // blast is a host-side birth on a client, dissolved by the explosion lane; the host's arrives as its own.
    2911,
    // aggro's Delay(2) resume: the "open" montage section and isAnim, entered after a relayed aggro.
    30,
    // "open" montage's finish notify: the sphere's collision refreshed and charge(true).
    578,
    // "close" montage's finish notify: isAnim false and the sphere's collision refreshed. A begin-overlap that
    // refresh re-fires lands in a refused entry (3766).
    837,
    // The montage proxies' delegates but the finish notify, four for each of the three sections ("open", "close",
    // "fire"): completed, blend-out, interrupted and notify-end. Each only copies the notify's name into a
    // temporary and returns.
    725, 753, 781, 6224,
    809, 980, 1008, 1036,
    1064, 1826, 1854, 1882,
};

constexpr EntryTable kTables[] = {
    {P::name::NpcClass_Zombie, kZombie, sizeof(kZombie) / sizeof(kZombie[0])},
    {L"npc_zombie_alienSummoner_C", nullptr, 0},
    {L"npc_zombie_corpse_C", nullptr, 0},
    {L"npc_zombie_skerfuro_C", nullptr, 0},
    {P::name::NpcClass_Fossilhound, kFossilhound, sizeof(kFossilhound) / sizeof(kFossilhound[0])},
    {P::name::NpcClass_GeomOcta, kGeomOcta, sizeof(kGeomOcta) / sizeof(kGeomOcta[0])},
};

constexpr size_t kTableCount = sizeof(kTables) / sizeof(kTables[0]);

// Each table's class name as an engine FName, resolved once (the way creature_event_relay.cpp's IsPerViewer holds its
// classes): a class is told from a table by two integers, never by rendering its name.
R::FName g_names[kTableCount];
bool     g_namesResolved = false;
bool     g_unresolvedSaid = false;

bool Same(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

bool NamesReady() {
    if (ResolveClassNames()) return true;
    if (!g_unresolvedSaid) {
        g_unresolvedSaid = true;
        UE_LOGW("npc_mirror_brain: the parked class names are not resolved yet -- no class reads as parked");
    }
    return false;
}

}  // namespace

bool ResolveClassNames() {
    if (g_namesResolved) return true;
    if (!ue_wrap::fname_utils::Resolved()) return false;
    for (size_t i = 0; i < kTableCount; ++i) {
        g_names[i] = ue_wrap::fname_utils::StringToFName(kTables[i].className);
        if (g_names[i].ComparisonIndex == 0 && g_names[i].Number == 0) return false;
    }
    g_namesResolved = true;
    return true;
}

const EntryTable* TableFor(void* graphClass) {
    if (!graphClass || !NamesReady()) return nullptr;
    const R::FName& name = R::NameOf(graphClass);
    for (size_t i = 0; i < kTableCount; ++i)
        if (Same(name, g_names[i])) return &kTables[i];
    return nullptr;
}

const EntryTable* TableNamed(const wchar_t* className) {
    for (const EntryTable& t : kTables)
        if (std::wcscmp(t.className, className) == 0) return &t;
    return nullptr;
}

bool ChainHasTable(void* cls, const EntryTable& table) {
    if (!NamesReady()) return false;
    const R::FName& want = g_names[&table - kTables];
    int hops = 0;
    for (; cls && hops < 16; cls = R::SuperStructOf(cls), ++hops)
        if (Same(R::NameOf(cls), want)) return true;
    return false;
}

}  // namespace coop::npc_mirror_brain
