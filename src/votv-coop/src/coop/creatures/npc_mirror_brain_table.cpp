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
    // step (a footstep sound and emitter); it spawns nothing.
    19440,
};

// npc_zombie_alienSummoner_C, npc_zombie_corpse_C and npc_zombie_skerfuro_C own a graph of their own and allow none
// of it: its entries are the landing, the destroy, the corpse's `died`, the summon timer and their resumes, and each
// spawns actors, starts a timer or moves the zombie (skerfuro's resume 30 starts a chain that ends in `doWalk`).

// fossilhound_C
constexpr int32_t kFossilhound[] = {
    // step: the footstep notify. A trace, a physical-material sound, a sound, and every fourth step the voice.
    8433,
    // the per-viewer loop: the camera-distance test and its event count, re-entering itself on a 0.25 s delay. Our
    // setup starts it (ue_wrap::fossilhound), and each client counts its own view, as the class does.
    ue_wrap::fossilhound::kPerViewerEntry,
    // ReceiveDestroyed: the `false` of the loop's `true` when the hound was in view, so the count comes back.
    10655,
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

// antibreather_C
//
// The class's BeginPlay, tick, per-viewer check and destroy run on a mirror; every other entry of its graph is refused.
// The four allowed, with what each does:
//   9848 BeginPlay: the mesh attached to the spring arm, the animation instance written into `anim`, the 1 s door timer,
//        the 0.25 s per-viewer timer and the 60 s mushroom timer, and `wander()` (all three land in refused entries).
//   4489 ReceiveTick: the animation's walk blend from the velocity the pose drive writes, the feet and the yaw. Its one
//        decision, the punch, runs only while `player` is valid, and the sight entry that writes it is refused here.
//   11016 checkVisible (the timer): `rendered` is the distance of THIS machine's camera 0 within 10000, and its event
//        count (lib::setEvent), so each viewer counts its own view (coop/world/creature_event_relay).
//   10320 ReceiveDestroyed: the `false` of that count when the creature was in view.
// Refused: 10278 wander (a move near player 0), 10283 the pawn-sensing sight (target, chase, the mushroom drop), 7861
// punch, 9701 dashing, 6360 findBlindSpot, 6207 playSound and 6322 its callback, 11011 openDoor, 10669 acquireMushroom,
// 11021 the hogger sphere (a gray boar or a fossilhound that touches it takes 1e8 damage), and the six move-to proxies'
// success and fail entries (3847, 4209, 7752, 7965, 8453, 10288). A cooked byte offset each: a game update moves them.
constexpr int32_t kAntibreather[] = {9848, 4489, 11016, 10320};

// centipedeHead_C (Wolfgang, and wolfgangJr_C below it, the pups: their own classes own no graph)
//
// BeginPlay, tick, the per-viewer loop and destroy run on a mirror, so a copy stands with its body: the begin play mints
// the segments (centipedeSegmet_C, each with its own begin play and nothing else), the tick lays them along the head's
// path, and the destroy ends them with the head. Each segment is an Actor with a mesh and no gameplay.
//   12599 BeginPlay: the segments, the five timers (kick 0.4 s, calc 0.5 s, checkDoors 1 s, customEvent 2 s and, for a
//        pup, bbyoffste, each landing in a refused entry), the per-viewer loop, and for a head born while a parentWolfgang_C
//        stands the eight pups, which a client refuses at the spawn (coop/creatures/centipede_pups).
//   12997 ReceiveTick: the rope of segments from the head's pose, its mesh's bob, `addMovementInput` (a mirror's movement tick
//        is off, so it moves nothing). Its target branches read `target`, which only calc writes, refused here.
//   463  the per-viewer loop's 0.25 s resume: the camera-distance test and its event count, each viewer's own.
//   15754 ReceiveDestroyed: the segments and the pups it holds, destroyed with the head, and the `false` of the count.
// Refused: 15846 kick (physics impulses on the props about), 15851 calc (the target, and the food and chip piles it picks),
// 15856 checkDoors (doors within reach), 16170 customEvent, 15889 bbyoffste, 14242 the mouth's overlap (it destroys the food
// it touches, and a heap of foods hatches Wolfgang's egg), and the four move-to proxies' success and fail entries.
constexpr int32_t kCentipede[] = {12599, 12997, 463, 15754};

constexpr EntryTable kTables[] = {
    {P::name::NpcClass_Zombie, kZombie, sizeof(kZombie) / sizeof(kZombie[0])},
    {L"npc_zombie_alienSummoner_C", nullptr, 0},
    {L"npc_zombie_corpse_C", nullptr, 0},
    {L"npc_zombie_skerfuro_C", nullptr, 0},
    {P::name::NpcClass_Fossilhound, kFossilhound, sizeof(kFossilhound) / sizeof(kFossilhound[0])},
    {P::name::NpcClass_GeomOcta, kGeomOcta, sizeof(kGeomOcta) / sizeof(kGeomOcta[0])},
    {P::name::NpcClass_Insomniac, nullptr, 0},
    // npc_arirFollower_C allows none of its graph: its begin play starts the loops that walk it to player 0, slap the
    // player it stands beside, take the item from that player's hand, and open the doors and lockers in its way. Each is
    // the host's follower's to decide; a copy shows the host's pose. Its children (the pig beater) run this graph too.
    {P::name::NpcClass_ArirFollower, nullptr, 0},
    // The classes below it own a graph beside its, which the follower's row does not catch, and each allows none of it.
    // The pig beater's begin play names the nearest gray boar its target, and its `targetDestroyed_d` re-picks one or runs
    // away, so both only feed the follower's loops. The slapper's begin play notes where it stands to put a stolen thing
    // back, and its `finished` returns that thing: a prop it was made to fetch, which the host's slapper alone moves.
    {P::name::NpcClass_AriralPigBeater, nullptr, 0},
    {L"arirSlapper_C", nullptr, 0},
    // npc_ariral_shooter_C allows none of its graph. Its begin play registers an event (lib::setEvent, which a client must
    // not raise for the host's), starts a 1 s sense timer that targets the nearest fire tank, gray boar or murder kerfur
    // among the client's own copies, and a 0.3-0.5 s fire timer whose body spawns an arirgun_shoot_c: a beam whose begin
    // play damages whatever implements the object interface within a metre or so of its hit and pushes the physics bodies
    // there. Its tick walks it toward that target. A copy shows the host's pose; the host's beams are the host's.
    {P::name::NpcClass_AriralShooter, nullptr, 0},
    // npc_arirGunStealer_C allows none of its graph: its begin play registers an event and sets it walking to the dropped
    // gun, or to player 0 once the gun is gone; its `punching` timer slaps any player in reach, opens lockers in its way and
    // shoves props; and it destroys the gun and throws a prop at the end. A copy shows the host's pose.
    {P::name::NpcClass_ArirGunStealer, nullptr, 0},
    // npc_funguy_C allows none of its graph: its begin play arms the 0.1 s sensing loop (the pawns in its see radius, from
    // which it runs away), the awake timer (the dig-up montage) and the despawn timer, and its move-to proxies wander it to a
    // random point of this machine's navmesh; its footstep notify and its landing mint a mushroom spawner or a dirt hole; its
    // radii read a mainPlayer and the knockout radius hits one. All of it is the host's duende's to decide; a copy shows the
    // host's pose and its end is the host's despawn.
    {P::name::NpcClass_Funguy, nullptr, 0},
    {P::name::NpcClass_Antibreather, kAntibreather, sizeof(kAntibreather) / sizeof(kAntibreather[0])},
    {P::name::NpcClass_Centipede, kCentipede, sizeof(kCentipede) / sizeof(kCentipede[0])},
    // The live Erie (eriePlushLive_C) allows none of its graph. Its begin play takes its collision off, starts the 30 s `stealing`
    // timer (it takes a prop from the floor in 10 m, and the evil kind throws grenades), the 120 s `reducerep` timer (its `rep` falls,
    // and at -100 or 100 it turns into the evil or good kind and a bomb bottle) and the 1 s `regen`; its move-to chain (`startmoving`)
    // walks it to a point of this machine's navmesh and picks a dance; its use (`actionOptionIndex` 7) makes the drag form and
    // destroys it, a feed (`playerUsedOn`) eats the player's held prop, fire turns it into a drag form or a chip pile, a slice into
    // the chop, and its end gives back the prop it stole. Each is the host's Erie's to decide; a copy shows the host's pose. The good
    // and the evil kind own a graph beside it (the kind's begin play and one custom event), which allows none either; the angry one
    // (npc_angryErieFlesh_C) chases, throws artillery and sets props alight, all the host's.
    {P::name::NpcClass_EriePlushLive, nullptr, 0},
    {L"eriePlushLive_good_C", nullptr, 0},
    {L"eriePlushLive_evil_C", nullptr, 0},
    {P::name::NpcClass_AngryErie, nullptr, 0},
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
