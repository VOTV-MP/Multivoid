// coop/world/event_fire_policy.cpp -- see coop/world/event_fire_sync.h. The replay policy alone
// lives here so the dupe matrix can grow a row at a time free of the sync logic's size budget.

#include "event_fire_policy.h"

#include <cstring>
#include <string>

namespace coop::event_fire_sync {

// The replay policy, the dupe matrix: every row's concrete output and which lane already
// carries it. The default is no replay. Replay only rows whose effect is a deterministic
// level, save or cosmetic flip that no existing lane delivers; replaying a lane-covered row
// would double-deliver. Keyed by name only: the few names living in both dispatchers have
// the same verdict either way, and the replay call still uses the received dispatch kind.
const char* const kReplayRows[] = {
    // Story and save flips (level-placed triggers; no lane):
    "treehouse_0", "treehouse_1", "treehouse_2", "treehouse_3", "treehouse_4", "treehouse_5",
    "break_RomeoSierra", "break_Victor", "break_Victor2",
    // Force-object appends (a save array the client's own dish scan reads; no lane):
    "looker_0-1", "looker_1-1", "looker_2-1", "looker_3-1", "looker_4-1",
    "arirSignal", "arirSpk", "picSignal", "peace",
    "arirSat_0", "arirSat_1", "arirSat_2", "piramid_sig",
    // Cosmetic or sound with no lane (the solar row's lights-dark converges with the light lane,
    // the same resulting state, echo-suppressed by its last-known prime):
    "solar", "call0",
    // Trigger-box scare arms, per-viewer scares by design; arming both sides is the correct coop
    // semantics (each player gets the scare on their own overlap):
    "toeStab", "falseEnter", "mann", "vent", "crys", "fakeGrays", "susArir",
    // Graffiti decal specials (a grime decal spawn; no lane):
    "arirGraff_0", "arirGraff_1", "arirGraff_2", "arirGraff_3",
    "arirGraff_4", "arirGraff_5", "arirGraff_6",
    // Effects on the machine's own player or screen; no lane carries them and none doubles. alienSounds
    // spawns three noiser_C around the local player, console writes into the machine's own terminal widget
    // (the empty line a client runs there is not forwarded), treehouseSleep and bedEvent teleport the local
    // player at its own next wake. bedEvent's bed is the host's alone: a client's body leaves the bed where
    // it is and the host's move rides the prop drive (coop/world/bed_event_gate):
    "alienSounds", "console", "treehouseSleep", "bedEvent",
    // Oil stains: eight spawners at fixed points, each leaving a grime decal where its trace lands (no lane):
    "oil",
};

struct NoReplayRow { const char* name; const char* lane; };
const NoReplayRow kNoReplayRows[] = {
    // Outputs already ride a lane (a replay is a double delivery):
    { "starRain", "event_cue lane (cue 0)" },
    { "arirFollower", "npc lane" },
    // The swarm's wisps ride the npc lane (the source-gated catch in npc_world_enum); a replay
    // would arm the client's own swarm trigger and double-spawn client-local creatures on top of
    // the mirrors:
    { "wisps", "npc lane (EX-catch; event-swarm wisp_C mirrored)" },
    // The pyramid's path is host-random (wander and chase timers), so a replay armed the client's
    // own trigger box and a client walk-in spawned a divergent client-local pyramid with
    // unmirrored wisps. The arrival comes by mirror: the world-actor pose stream, npc-lane wisps
    // and the pyramid sync's brain suppression and gather relay.
    { "piramid", "piramid mirror lane (WA pose + piramid_sync brain/gather)" },
    // The arm runs the level-placed arirShipAppear_C tower's runTrigger: `appear(true)` fades the tower in and binds the photo
    // reward, and its own tick hides it again when a camera looks at it. A replay would arm the client's own tower on its own
    // clock and run the second `appear` of a joiner's; the host's `appear(dir)` crosses as a cue instead (coop/world/
    // event_world_sync: 132 shown, 133 hidden, and a joiner is told while it shows), which a client's own tower follows, its
    // mail loop refused (spawn_authority). arirShip_C, the warp-in ship, is not this event's output: the Halloween
    // ufoAlienDropper_C's begin play spawns it, on the world-actor lane:
    { "arirShip", "arirShipAppear_C cues 132/133 (event_world_sync); arirShip_C is the Halloween dropper's, world-actor lane" },
    // The arm only readies a level box; the obelisk falls when a player walks into the host's. The host's
    // obelisk_C is a world-actor copy on a client, which runs the fall, the shakes and the punch of its own
    // player and mints none of the props (coop/world/obelisk_copy); the rocks and the obelisk prop are the
    // host's, on the prop lane. A replay of the arm would let a client's own box fire a second one:
    { "obelisk", "obelisk_C world-actor copy (coop/world/obelisk_copy); its props ride the prop lane" },
    // earthTp fires newsky_C.tp, a 106 s latent chain on the sky actor: setEvent(true), a 2D sound, the black hole's light and
    // skyVisibility(true) at 20 s, a mail, a second sound at 86 s, both undone at 106 s and two mails. Run again it would stack
    // the event count (its delays are one action each) and mail twice, and a joiner's would start the chain over, so it is
    // not replayed: the flip rides the sky lane (SkyStatePayload.blackHole, once a second and at a joiner's world-ready), the two
    // sounds are cues 130 and 131 (event_world_sync), and the mails ride the host's append feed:
    { "earthTp", "SkyState.blackHole (sky_sync) + sound cues 130/131 (event_world_sync); the mails ride the host append feed" },
    { "vehtp", "atv lane" },
    { "picnic", "prop lane" }, { "destroyPicnic", "prop lane" },
    { "enasus", "prop lane" }, { "enacros", "prop lane" },
    { "cookier", "prop lane (armed prop)" }, { "paperGray", "prop lane (armed prop)" },
    { "arirEgg", "prop lane (armed prop)" },
    { "lightswitch", "device lanes" },
    { "keypadGuess", "device lanes" },
    // atvExplode sets car.trap on the host's own ATV; the ATV lane carries that flag to every copy in the stamp's
    // bit 4, so the event itself stays host-only (a client's car need not be the host's). `zapped` has no lane:
    { "atvExplode", "car.trap rides the ATV lane's stamp (zapped has no lane)" },
    // Host-local by design:
    { "agrav", "prop drive (agrav_sync streams the host's nearest lifted props and corrects the rest at the end; a replay would lift its own and break a generator)" },
    // Creature and save-actor spawns, host-only until mirrored. The crawler, the egger's eggs, the
    // grays, the tentacle balls and the eventer's ships, saucers, salt pile and Soltomia craft are mirrored; the eventer's other
    // outputs are not (its catch enrols only what a client's copy is kept inert for or has no
    // gameplay to run, npc_world_enum), so they stay host-only:
    { "ventCrawler", "npc lane (allowlisted; eventer EX-catch)" },
    { "ventKnocker", "world-actor lane (eventer EX-catch; kocker_C's knocks play on each peer whose player enters its box)" },
    { "tentacleBalls", "npc lane (follower EX-catch)" },
    { "morningGay", "world-actor lane (eventer EX-catch; morningUfo_C pose-driven, shown once it moves)" },
    { "borgRozital", "world-actor lane (eventer EX-catch; rozitBorg_C flies by its own tick on each peer)" },
    { "graysforest", "npc lane (controller EX-catch)" },
    { "graystank", "world-actor lane (eventer EX-catch; ufoDropper_tank_C or _pig_C saucer, the generator break rides the power lane)" },
    // The Lantern Prank. arirBusterSpawner_C's whole begin play is K2_DestroyActor on itself, and its child actors outlive
    // it: the blinking lantern (prop_lamp_blinker_C) and the notepad (prop_notebook_busterSpawner_C) are minted by the host
    // and cross on the prop lane (the event-fire drill copies both on a client), so a replay would mint a second pair. No creature is made by the arm.
    // The door-battering Ariral is the arirSlapper (buster = true) that the notepad's use mints, in the notepad's own
    // `actionOptionIndex`: it is bound to the player who read it (the batter walks to player 0, hits the doors in its way
    // and then him), so it stays per player and is not a spawn source:
    { "arirBuster", "prop lane (lantern and notepad survive the spawner); the battering slapper is the reader's own, per player" },
    { "eggvasion", "npc and world-actor lanes (eventer and egger EX-catch)" },
    { "boarwar", "boars: prop lane + boar_sync; fire tanks and shooters: mirrored; the saucer: ufo_shield_sync" },
    { "soltoClean", "world-actor lane (eventer EX-catch; soltomiaCleaning_C is a husk on a client: its begin play, box overlap and destroy are refused, the ATV, doors and sponge come from the host's lanes, and solto_sync shows it and plays its leaving on the host's cues)" },
    { "salt", "world-actor lane (eventer EX-catch; saltpile_C has no body but the dispatcher's dropHeart, so a client's copy is inert; the heart rides the prop lane)" },
    { "rozitalHole", "the hole's room, kavotias and igetis: mirrored; its props: prop lane" },
    { "dreambase", "save-actor spawn (no lane)" },
    { "fallbody_0", "world-actor lane for the ufoDropper_body_C saucer (eventer EX-catch); the drop rides the prop lane" },
    { "fallbody_1", "world-actor lane for the ufoDropper_body_C saucer (eventer EX-catch); the drop rides the prop lane" },
    { "fallcar_0", "world-actor lane for the ufoDropper_car_C saucer (eventer EX-catch); the drop rides the prop lane" },
    // The prank layer (host-local RNG; thrown-prop outputs ride the prop lane):
    { "food", "prank special (prop lane)" }, { "drive", "prank special (prop lane)" },
    { "atvFuel", "prank special (prop lane)" }, { "atvFix", "prank special (prop lane)" },
    { "poisonFood", "prank special (prop lane)" }, { "expDrive", "prank special (prop lane)" },
    { "cookiebox", "prank special (prop lane)" }, { "trashPiles", "prank special (prop lane)" },
    { "vaccine", "prank special (prop lane)" },
    { "begos", "prank special (prop lane)" }, { "gascans", "prank special (prop lane)" },
    { "bombBox", "prank special (prop lane)" },
    // The three spawners are the host's, and a client has none and needs none: each is a stationary actor whose graph throws props, so
    // what a client sees is the prop lane's. hillRollerSpawner_C's box overlap mints the cart (a pallet, four wheels, a spotlight head, a
    // paper alien) and rolls it down the hill (seven props announced, coasted and copied on a client in the
    // same second); its nails (four nail_C and four nail_ax_C, which hold the cart's pieces together: none stood on the client) and its rumble have no lane. alienJump_C is shown by a box overlap
    // and, when a clear line runs to player 0's camera, throws its planks and a paper alien and hides (eleven props cross);
    // the alien's scream is attached to the host's prop and has no lane. rockThrower_C (three at fixed points, no mesh) throws a
    // prop_rock_C every 2 to 6 s while player 0's pawn stands in its box, 4 to 5 times (read from its graph: the box overlaps only the host's player,
    // which a drill may not place, so no rock was thrown; each rock is a prop born as the cart's are). All three aim at the host alone.
    { "rockThrow", "prank spawner (host only, no copy; its rocks ride the prop lane)" },
    { "hillRoller", "prank spawner (host only, no copy; the cart's props ride the prop lane, its nails and rumble have no lane)" },
    { "alienJump", "prank spawner (host only, no copy; the planks and the alien ride the prop lane, its scream has no lane)" },
    { "trashBase", "prank spawner (trash_fling_sync coasts the props arirTrasher_C flings and announces its clumps)" },
    // The random prank's wrapper: the host picks the special, which arrives as its own fire.
    { "ariralPrank", "prank wrapper (host RNG; the picked special is its own fire)" },
};

// The interaction rows: the row's entire effect is the prank special, host-local RNG.
static bool IsPrankRow(const std::string& n) {
    return n.rfind("arirInteraction_", 0) == 0;
}

// An event's name is an FName, which renders in whichever casing the process registered first ("Console" for the
// row "console"), so a row is matched without regard to case.
static bool SameName(const std::string& name, const char* row) {
    return _stricmp(name.c_str(), row) == 0;
}

int ReplayVerdict(const std::string& name, const char** laneOut) {
    for (const char* r : kReplayRows)
        if (SameName(name, r)) return 1;
    for (const auto& nr : kNoReplayRows)
        if (SameName(name, nr.name)) { *laneOut = nr.lane; return 0; }
    if (IsPrankRow(name)) { *laneOut = "prank special (host-local RNG)"; return 0; }
    return -1;
}

}  // namespace coop::event_fire_sync
