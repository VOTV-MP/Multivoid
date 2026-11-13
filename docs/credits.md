# Credits — the full ledger

Multivoid has no QA department and no team. Almost everything in it beyond the
maintainer's own work arrived from outside, in one of three forms: **code**,
**reports**, and **review**. This file is the complete record of all three — who
contributed, what it turned out to be, and what shipped because of it. The
[README](../README.md#credits) and the [website](https://multivoid.dev/#credits)
carry a one-line-per-person summary; the detail lives here so those stay readable.

**The admission rule is the same for all three kinds: if it changed the mod, it
gets a row** — no gatekeeping on how polished it was. A log pack saying "it felt
wrong around here" has repeatedly been worth more than a tidy description, and the
largest single change in this project's history came from someone saying, in
public, that a decision in it was mistaken.

**How to land in here:** open a pull request, or report anything on
[Discord](https://discord.gg/bA6tGBvGMN) or in
[GitHub issues](https://github.com/VOTV-MP/Multivoid/issues). What to attach:
**[install.md](install.md)**.

---

## The ledger

Grouped by kind; within a group, largest or most recent first. Commit counts come
from `git shortlog -sne` and fold each person's identity variants together.

| Who | Kind | Contribution | Landed |
|--|--|--|--|
| **pelmentor** | code | Architecture, direction, releases — the whole mod | 831 commits |
| **Claude** (Anthropic) | code | Implementation, across the whole mod | 1,367 commits |
| **Tarangok** | code | KO respawn, live skin preview, held-prop visibility, container extraction | 5 commits |
| **hediiiqq** | code · report | Dish mirror interpolation; a CI gate failing every build on the unfetched MTA submodule ([#10](https://github.com/VOTV-MP/Multivoid/issues/10)) | 1 commit · #10 |
| **arigalit** | code · report | ATV seat contention ([#9](https://github.com/VOTV-MP/Multivoid/pull/9)); join-time prop-count divergence; the grappling-hook lane ([#16](https://github.com/VOTV-MP/Multivoid/pull/16)) — four of its decisions are in the shipped lane; the drone leave timer, the desk ping's scan progress, the sleeping-bag transitions, the notebook's shared page, the revive's fire, the cord's plug and the coffee machines ([#50](https://github.com/VOTV-MP/Multivoid/pull/50)); the Geom Octa's place in the NPC lane ([#48](https://github.com/VOTV-MP/Multivoid/pull/48)); an admin's crematorium order ([#57](https://github.com/VOTV-MP/Multivoid/pull/57)) | 15 commits · 2 co-authored · unreleased |
| **huoyan1231** | code · report | CI and automated builds; the b125 host-log pack | 2 commits · b134 |
| **Marlore** | code · report | A nine-symptom pass over the trash carry ([#29](https://github.com/VOTV-MP/Multivoid/pull/29)). Four were real defects and are fixed at the root, co-authored: a refused grab that was never answered, a pile that looked different on every peer (865 of 871 measured), a thrower refused every later grab, and a fallen player who kept carrying | 6 commits · unreleased |
| [**Wigard**](https://github.com/wigarddev) | code | A guest of a sandbox host came up in story ([#32](https://github.com/VOTV-MP/Multivoid/pull/32)) — the transfer header had carried the host's game mode from the start and the host filled it with a constant. Then seven more, each measured red and green before it was sent: a build that did not link on VS 2022 ([#30](https://github.com/VOTV-MP/Multivoid/pull/30)), the shower's mirrored toggle running the wrong verb ([#31](https://github.com/VOTV-MP/Multivoid/pull/31)), a drive left frozen in the port on the far peer ([#33](https://github.com/VOTV-MP/Multivoid/pull/33)), a pile a client bags that the host never learns of ([#34](https://github.com/VOTV-MP/Multivoid/pull/34)), a client's upgrade purchase that charged nobody ([#35](https://github.com/VOTV-MP/Multivoid/pull/35)), a prop whose record changes in place telling no one ([#36](https://github.com/VOTV-MP/Multivoid/pull/36)), the bay window's sponge dabs reaching no other peer ([#37](https://github.com/VOTV-MP/Multivoid/pull/37)), and a client's press of the drone console reaching a drone that cannot fly ([#38](https://github.com/VOTV-MP/Multivoid/pull/38)). Then a disc the host ejects that reached the others blank, with the disc drill that proves it ([#39](https://github.com/VOTV-MP/Multivoid/pull/39)), and a desk repaint whose diagnosis matched a fix main already carried ([#40](https://github.com/VOTV-MP/Multivoid/pull/40)) | 13 commits · unreleased |
| [**Shinobu2**](https://github.com/Shinobu2) | code | About twenty lanes in one request ([#45](https://github.com/VOTV-MP/Multivoid/pull/45)), adopted lane by lane: a newer save record replacing a parked one, a thrown container that kept its contents, damage and fire refused on a peer's puppet with its flame shown, the story-event creatures mirrored, an event fire crossing at the verb it ran in, a client's point sack paid by the host, the server repair's reward, the runtime ATV spawn, a joiner's daily task and nine more | 23 commits · unreleased |
| [**Slintchen**](https://github.com/Slintchen) | code | The request that the mod's UI speak the player's language ([#41](https://github.com/VOTV-MP/Multivoid/pull/41)): its Chinese pack, in as authored, and its translator's guide and system-face merge for CJK, carried into the translation layer with Slintchen as co-author | 1 commit, 2 co-authored · unreleased |
| [**archhn0madd**](https://github.com/archhn0madd) | code | Rejoin without a relaunch — the boot poll answered from the dying world | 1 commit |
| [**AwazonDotCom**](https://github.com/AwazonDotCom) | code | A floppy row lost its line breaks on the way to a client, so the server-data row a host's dish writes, hash code included, did not parse there; and a drive rack written by two lanes at once ([#53](https://github.com/VOTV-MP/Multivoid/pull/53)) | 2 commits · unreleased |
| [**gust138**](https://github.com/gust138) | code | A drive box or reel case changed twice in quick succession lost its last change on the other peers, and a client's coin pickup credited and destroyed the coin before the host agreed ([#64](https://github.com/VOTV-MP/Multivoid/pull/64)) | 5 commits |
| [**Ghezra**](https://github.com/Ghezra) | code | A remote player's own body cut a silhouette out of its flashlight beam on every other screen ([#71](https://github.com/VOTV-MP/Multivoid/pull/71)); an empty battery left the other peers' copy of a player lit ([#79](https://github.com/VOTV-MP/Multivoid/pull/79)); a link fix for the Steam networking header ([#66](https://github.com/VOTV-MP/Multivoid/pull/66)); the desk's signal view bounds and its graph or spectrogram mode staying on one machine ([#81](https://github.com/VOTV-MP/Multivoid/pull/81)); a client's dish slewing in silence but for one motor sound ([#82](https://github.com/VOTV-MP/Multivoid/pull/82)); a client never learning that the host saved, or that a save failed ([#69](https://github.com/VOTV-MP/Multivoid/pull/69)); spills that landed apart on each peer ([#74](https://github.com/VOTV-MP/Multivoid/pull/74)); a vent crawl only its own host heard and saw ([#78](https://github.com/VOTV-MP/Multivoid/pull/78)); two players holding one prop at once ([#68](https://github.com/VOTV-MP/Multivoid/pull/68)); a can opened or eaten from on one machine only ([#67](https://github.com/VOTV-MP/Multivoid/pull/67)); a food that cooled, ripened and burnt on each peer by itself ([#76](https://github.com/VOTV-MP/Multivoid/pull/76)); deer, walking trees, dirt holes and beehive branches rolled again on every peer ([#83](https://github.com/VOTV-MP/Multivoid/pull/83)); plants that grew, ripened and died differently on each peer ([#80](https://github.com/VOTV-MP/Multivoid/pull/80)); a woodchipper whose fuel and switch each peer ran on its own ([#88](https://github.com/VOTV-MP/Multivoid/pull/88)); a disc full of server data that stopped syncing past 32 KB, and a module row a client paid for on its own machine alone ([#86](https://github.com/VOTV-MP/Multivoid/pull/86)); a toolgun's freeze, unfreeze or make-static that changed the pressing machine's copy only ([#85](https://github.com/VOTV-MP/Multivoid/pull/85)); a walking mannequin every peer rolled and drove by itself, and that froze on anyone's gaze ([#84](https://github.com/VOTV-MP/Multivoid/pull/84)); the message kind's one byte nearly spent ([#117](https://github.com/VOTV-MP/Multivoid/pull/117)); an MRE a client opened that rolled its contents on that client alone ([#108](https://github.com/VOTV-MP/Multivoid/pull/108)); a zip disk a client dropped in the drone sack, filed on that client only while the host copy was lost unsold ([#111](https://github.com/VOTV-MP/Multivoid/pull/111)); six copies of the timer clear made one ([#105](https://github.com/VOTV-MP/Multivoid/pull/105)); a drill's players kept fed and rested ([#100](https://github.com/VOTV-MP/Multivoid/pull/100)); empty equipment slots a joiner could never fill ([#114](https://github.com/VOTV-MP/Multivoid/pull/114)); a knocked-down pinger that kept the desk ([#97](https://github.com/VOTV-MP/Multivoid/pull/97)); an ATV's fuel, repairs, modules, lights, brake, turbo and horn that each peer changed on its own copy ([#89](https://github.com/VOTV-MP/Multivoid/pull/89)) | 28 commits · unreleased |
| **Moddy** | review · design | The architecture and documentation review that became the UE4SS move; the public UE-Modding-Tools pointer that became the blueprint-CFG rung and the migration scanner (patternsleuth); and design published for [Relay](https://github.com/modestimpala/Relay), Moddy's Blueprint networking API for VOTV ([Thunderstore](https://thunderstore.io/c/voices-of-the-void/p/Moddy/Relay/)), in its README and the [README Blueprint](https://blueprintue.com/blueprint/g3s09x9c/) that README links: the watch surface on a Blueprint function, a watch that reads the parameters before the call and can cancel it, and one that only observes after it, which became the script-body gate; the readable join reason and stable diagnostic codes, which became the join screen's named steps and the end-reason codes; the list of cheap edge protections, of which two were missing here: a per-source limit on connections and a private access list on the identity key file; the rule columns that make an actor's own save record its spawn payload, the general form of what this project's prop save-data work was building case by case; the client-only `Quiesce` column, which made this project state its parking rule once and read every park against it; the one paragraph on container handling, which made it write down and measure its own container invariants; the note that a watch on a parent class misses a child's override, which sent us to audit every hook we install, which found three seams that had never installed and two verbs called on the wrong class; and the list of what a returning player's profile covers, which is the list this project's per-player profile now carries | b122 · b143 · 2026-09-02 · b153 · b157 · b160 · b161 · b167 |
| **SentientYeet** | review | The substrate critique that re-opened the loader decision | b143 |
| **Violet** | report | ~9 FPS for a friend joining on Linux — five separate defects behind it | b134 |
| **decodinatorX** | report | Couldn't type at the SAT console — `T` kept opening chat | b133 |
| **gediao** | report | The b125 host-log pack, with huoyan1231 | b134 |
| **doctaaaaa** | report | A ten-item field pack on the released build, of which four landed on open work: a floppy disc lost when it is retrieved from a signal server, a recorded signal lost when the disc changes hands, the power chain, and the trash-pile cost. The disc pair is now root-caused — the box on the other machine takes the disc back the moment it appears | b150 · root-caused, unreleased |
| **SirWilliam** | report | Rejoining a session requires a full relaunch | fixed, unreleased |
| **thewittyrobin** | report | Doors that never open for the other peer, twice ([#17](https://github.com/VOTV-MP/Multivoid/pull/17), [#18](https://github.com/VOTV-MP/Multivoid/pull/18)). Both proposed mechanisms measured out differently, and both reports are of one real defect: on the public build a door is addressed by a key the game re-mints per process | root-caused, unreleased |
| [**ElvardFIC**](https://github.com/ElvardFIC) and [**AquaFishhh**](https://github.com/AquaFishhh) | code | The shape of the food lane, from Watchers of the Void: the host as a food's one author, a client's bite as an intent, a held food's condition for the copy others show | food lane |

---

## Code contributions

Community commits are adopted with their **original authorship preserved**
(`git log --author=<name>` shows exactly what each person wrote).

### Tarangok
- **KO respawn** (`death.ko_respawn`): the death lane — the config surface, the
  KO/respawn shape, and the first attempt at answering VOTV's kick-to-menu
  permadeath. The mechanism has been reworked twice since (the death section of
  `docs/players.md` is where it stands); the lane and the idea are theirs.
- **Live mannequin skin preview**: hovering a skin in the F1 menu shows it on a
  real in-world mannequin.
- Cross-peer held-prop visibility (clients now see props carried by other
  clients, not just the host's).
- Container extraction (a client-extracted item now reaches the host's world),
  and the author-side volume re-derive.
- Duplicate keyed props on a joining client (the double starting suitcase).

### hediiiqq
- Dish mirror interpolation: the 4 Hz dish pose stream now glides through a
  proper lerp window instead of snapping every 250 ms.
- **A CI gate was failing every build**
  ([#10](https://github.com/VOTV-MP/Multivoid/issues/10)): it called ten MTA
  citations dead when the only thing wrong was that `reference/mtasa-blue` is a
  submodule the workflow deliberately never fetches. The report did the whole
  diagnosis -- it located the asymmetry (check B already skips an absent corpus
  and says so; check A did not), quoted the code's own reasoning back at it,
  named why the timing hid it (the gate landed 2026-08-29, the last green build
  was 2026-07-31), and argued AGAINST the easy allowlist fix because it would
  permanently stop checking those line numbers for anyone running the gate
  locally with submodules populated. Fixed exactly as suggested.

### arigalit
- **The drone leave timer** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its first piece): a
  client pressing the garage console's leave-timer face now sets the host's drone, and every peer sees
  the setting. The commit is theirs as written; the console's click and the drill's timer leg were
  added beside it.
- **The desk ping's scan progress** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its second piece):
  while one player pings coordinates at the main desk, every other peer now sees the coordinate lines
  fill in stage by stage. Their two commits are kept as written (the notebook half of the second waits for
  the notebook piece); a measurement, the pinger's identity on the wire and the observer checks were added
  beside them.
- **The sleeping bag and mattress** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its third piece): a
  bag a player unrolls or folds stays on the other peers (FI-11). The host runs the
  game's own transition for a client and the unrolled bag, which the game never keys, gets a key. Their
  commit is kept as written; the bunk-bed mattress, the press-side watch and the drill were added.
- **The notebook** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its fourth piece): what one player
  writes in a notebook is now what every peer reads, one editor at a time. Their two commits are kept
  as written; the text was then routed through the notebook's own save record, which already crossed,
  and the editor claim, the write rule and the drill were added beside them.
- **The revive's fire** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its fifth piece): a player who
  died burning came back still burning. Their commit is kept as written; the fire is now put out with the
  game's own verb, and a drill sets the player alight, kills it and checks the revived body.
- **The cord's plug** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its sixth piece): a cord one player
  plugs into a socket is now plugged on every peer, a joiner included. Their commit is kept as written; the
  link was then made the host's (a client claims its own plug and the host answers), and the drill and the
  forged-message checks were added beside it.
- **The coffee machines** ([#50](https://github.com/VOTV-MP/Multivoid/pull/50), its seventh piece): a client's press of
  the coffee vending machine or the coffee maker now takes its points and brews its cup, once, on every peer. Their
  commit is kept as written; the press was then made a request the host performs, the maker's state its record
  published by the host, and the drill and the forged-message checks were added beside it.
- **The Geom Octa** ([#48](https://github.com/VOTV-MP/Multivoid/pull/48)): the creature that falls out of the
  night sky and fires a laser is now the host's, and every peer sees the same one. Their commit is kept as
  written: the Octa on the NPC allowlist, the gamemode's night roll as a spawn source and the dev menu's spawn
  button. The rest of the pull request was rebuilt: the mirror's decisions are parked and the host's attack is
  relayed into each client's own Blueprint, a client's own roll is refused, a joiner mid-attack is replayed the
  attack, and the drill was added beside it.
- **The grappling-hook lane** ([#16](https://github.com/VOTV-MP/Multivoid/pull/16)): the first
  implementation of hook and rope visibility across peers. It could not be merged — a mirror is a
  live `hook_C`, `hook_C` is an `actor_save_C`, and the game's save walk collects by that interface,
  so every mirror standing on a peer would have been written into that player's save. The divergence
  was ours to own: the design it had to match is not in this repository at all, so no outside
  contributor could have read it. Four of its decisions are in the lane
  that shipped, and are credited on the commits: payload validation before anything is applied, a
  connect replay so a joiner is not left without the hooks already standing, the perf-bucket walk
  timer, and a mirror given neither collision nor tick. The `ue_wrap/actors/hook` split beside the
  lane is theirs too — the design names no wrapper at all.
- **ATV seat contention** ([#9](https://github.com/VOTV-MP/Multivoid/pull/9)): a
  peer walking up to an ATV somebody else is already driving is denied at the
  input seam, instead of both engines running vehicle physics and fighting over
  the body.
- **The admin's crematorium order** ([#57](https://github.com/VOTV-MP/Multivoid/pull/57)): an admin can
  send a player to the base cremator from the admin panel or the scoreboard. Their commit and its
  translations are kept with them as author. On top, the cremator became the host's machine: its lever
  run reaches every peer, joiners included, a client's pull of its handle is a request the host judges,
  the burn's hits are dealt in the burning player's own game, and a client's copy burns nothing. The
  order became the `/cremate` command with its own permission and exempt nodes; the host runs the
  machine's own lever instead of igniting the player itself, judges the trip from the player's poses
  and the burn volume, and kicks the cremated player with an end reason of its own.

### Marlore
- **The trash carry, as a player met it** ([#29](https://github.com/VOTV-MP/Multivoid/pull/29)):
  two commits written against a live session, naming nine things that went wrong. The patch
  could not be merged as written (it sits on the history withdrawn in September, and each hunk
  was measured against the tree before anything was taken), so every symptom was re-run from
  our own chair with a drill, and what reproduced was fixed where it starts. Four did, and the
  commits carry their name:
  - **A refused grab was never answered.** The client's request stayed pending, so the next
    grab of that pile by anybody else read as its own confirmation and its use presses became
    throws of a clump it did not hold. The host now answers every refusal, to the one who
    asked, by request number.
  - **A pile looked different on every peer.** The pull request pointed at the pile's rotation;
    the census that followed measured 865 of 871 piles turned differently on the two machines.
    The game re-rolls the visible mesh's turn and size on every construction and saves neither,
    so the host's look now travels with the pile.
  - **A thrower refused every later grab.** Their fix ended the hold at the throw; that half is
    what shipped. A hold used to last until the clump landed as a pile, and a clump that comes
    to rest on a box never does.
  - **A player who fell kept carrying.** The game drops what a fainting player holds, but a
    client's clump is in its puppet's hand on the host, where that drop cannot reach.
  Beside those: a trash mirror whose GC pin fails is still kept as one we made (the
  pull request destroyed it; the defect under that was a lost record), and the drill that watches
  a host-thrown clump from the client exists because their report asked the question.
  One did not reproduce on the current tree on either path (a thrown clump frozen in the air),
  and three turned out to be the tree already doing the right thing; the pull request thread
  says which and why.

### huoyan1231
- CI and automated builds (`.github/workflows`).

### Wigard
- **A joiner loads in the host's game mode** ([#32](https://github.com/VOTV-MP/Multivoid/pull/32)).
  A world's mode — story, sandbox, tutorial and five more — is one byte on the game instance, and
  the game recovers it from a save's name prefix. A joiner's downloaded slot is named for the
  session, not for a mode, so the save-transfer header has always carried the host's ordinal for
  the joiner's load to force; the host filled it with a hardcoded story. Every guest of a sandbox
  host therefore came up in story: story events on, and no cheat menu, noclip or spawn menu for
  that player alone, in a world where the host had all three. The host now reads and sends its own
  mode, and the joiner refuses an ordinal that names no mode. Adopted with authorship preserved.

- **Seven more lanes**, each opened with its own red and green runs
  ([#30](https://github.com/VOTV-MP/Multivoid/pull/30), [#31](https://github.com/VOTV-MP/Multivoid/pull/31),
  [#33](https://github.com/VOTV-MP/Multivoid/pull/33) – [#38](https://github.com/VOTV-MP/Multivoid/pull/38)),
  adopted with authorship preserved. Three include-only lines that make the mod link under the
  VS 2022 toolset BUILDING.md names. The shower's mirrored toggle, which wrote the bit and asked for
  a dirt repaint instead of the water. A drive pulled out of the desk's port on one peer, left
  frozen in the port on the other, where the prop lane drops every pose of the hand carrying it —
  with a two-peer drill that shows it. A pile a client bags with a folded bag or a roll: nothing
  crossed at all, and the host's world and save kept the pile. The eighteen upgrade levels, which
  rode only the transferred save, and a client's purchase, which debited the client and left the
  group's level untouched. A drive box and a tape reel case whose record changes while they sit
  there, which reached nobody until this lane watched each class's own refresh verb. The bay
  window's dirt, a render target wiped one sponge dab at a time, observed at the native draw and
  replayed on every peer. And the garage console's keyboard, which pressed a client's own mirrored
  drone, the one whose flight tick is suppressed.

- **A disc the host ejects reaches the others with its files** ([#39](https://github.com/VOTV-MP/Multivoid/pull/39)),
  with the disc drill that measured it red and green: the host's spawn drain now publishes the save record
  of a birth it adopts, and a disc another peer holds is refused at a slot the watching peer only mirrors.
  Adopted with authorship preserved; the drill now waits on readiness. And a desk repaint ([#40](https://github.com/VOTV-MP/Multivoid/pull/40))
  whose diagnosis matched, exactly, a fix main had landed hours before the request.

### Shinobu2
- **About twenty lanes in one request** ([#45](https://github.com/VOTV-MP/Multivoid/pull/45)), several written
  with AI tools, each re-derived against the tree it landed on and run on the two-peer rig before it was taken.
  Adopted one commit per lane under Shinobu2's name, each carrying the lane's net change: a newer save record
  that always replaces the parked one -- whose landing exposed, and so got fixed, a wait of ours that ended
  when a record arrived instead of when it landed; a held mirror the tracker knows; no prop destroy deferred to
  a load tail on the host; the clock panel reading a game-thread snapshot; a peer puppet's run-ending travel
  reviving nobody; a driven prop yielding to a hand at once; the roach scale and the wisp montage's real
  parameters; a per-process log for a second game on one install; libopus on the static runtime; a joiner's
  daily task; a SAT console line answered in its own context; a falling host birth streaming its pose; a
  client's point sack paid by the host; the server repair's reward paid with the fix; the runtime ATV spawn
  waiting for a ready world; a thrown container keeping its contents; damage and fire refused on a peer's
  puppet and its flame shown; the story-event creatures mirrored; and an event fire crossing once, at the
  verb it ran in. One more lane found a real hole whose root was ours, a drive row the host could write
  half-way, fixed by us with credit; a few were declined or parked, each with its reason in the reply.

### Slintchen
- **The mod's UI in the player's language** ([#41](https://github.com/VOTV-MP/Multivoid/pull/41)): a layer
  that lets a translator ship a language without a rebuild, with a finished Chinese pack, a system-face merge
  so CJK text renders, and a guide for translators. The layer was rebuilt on gettext catalogues with contexts
  and plural forms, wrapping only the text a player reads (the request had also wrapped engine names a
  reflected call looks up). The Chinese pack is in under Slintchen's name as authored; its conversion to the
  catalogues, the guide and the Chinese system face are commits of ours co-authored with Slintchen.

### archhn0madd
- **Rejoin without a full relaunch** — the fix for SirWilliam's report below.
  After a quit-to-menu the dying world and its ragdolled `mainPlayer_C` stay in
  `GUObjectArray` until the GC purge, and both of the boot poll's "where are we?"
  reads answered from that dead world: the corpse read as *in gameplay*, so a join
  booted into a world that was never loaded, and the dying world's `untitled` name
  read as *already loading*, so the `open` was never issued. Both reads now go
  through `world_identity` — the module that exists precisely because a dying
  world's actors outlive it — under one owner, `SurveyBootWorld`. It also
  un-strands a host trying to re-host after a death-flee.
  Contributed on the fork [Multifoid](https://github.com/archhn0madd/Multifoid);
  adopted as `engine_save.cpp`'s `SurveyBootWorld` with authorship preserved.

### AwazonDotCom
- **A floppy row's line breaks** ([#53](https://github.com/VOTV-MP/Multivoid/pull/53)): the floppy slot,
  floppy box and laptop buffer lanes encoded their strings with the chat encoder, which drops control
  characters, so the host's several-line server-data row reached a client on one line and the game could
  not find its type. The commit, authored as DexxInTime, is kept as written; the text codec now owns the
  three lanes' strings both ways, and the laptop and floppy drills carry rows with line breaks to prove it.
- **A drive rack with one author** (the same request): a client's drive rack was written by the rack lane and
  by the save-record lane, whose record the rack lane would read as the player's takes and sets. The commit
  claims the rack from the save-record lane and holds a joiner's rack canonicals through its join; its
  several-rows guard was taken out, and the host now sends a rack it first sees mid-session, with a drill
  that spawns a rack holding rows to prove it.

### gust138
- **A box's last change** ([#64](https://github.com/VOTV-MP/Multivoid/pull/64)): the record lane published a
  drive box or reel case at most once per 300 ms and dropped the rest, so a second change soon after the first
  never reached the other peers. Every change now publishes. The commits are kept as written; a two-peer drill
  opens a box and takes its drive about 380 ms apart, and its red, with a publish window restored, ends the far
  box with the drive still in.
- **A coin picked up by a client** (the same request): a client's E press credited the coin and destroyed it on
  that client before the host answered. The press now forwards and is cancelled, and the host credits it. The
  drill counts no local credit on the client and the same balance on both peers; its red counts one.
- **A balance nobody took** (the same request): the host now resends its balance until a peer accepts it.

---

### Ghezra
- **A beam with a hole in it** ([#71](https://github.com/VOTV-MP/Multivoid/pull/71)): a remote player's flashlight cone starts inside its head, and the game
  turns shadow casting off on a real player's two body meshes but a puppet kept the class default, so its body
  cut a flickering silhouette out of its own beam on every other screen. The puppet's spawn now switches both off
  through the engine's setter. The commits are kept as written; the beam drill logs the flags, `CastShadow=1` on
  both meshes without the fix and `0` with it.
- **A battery that ran out for one peer only** ([#79](https://github.com/VOTV-MP/Multivoid/pull/79)): at empty the game's tick clears the flashlight bool and
  hides the light without calling a function, so nothing went out and the others' puppet stayed lit. The sender
  now compares the bool with what it last sent. Kept as written; the drill was made to read only its own run's
  signals and to wait on the game instead of fixed delays (the second of two runs in one folder had judged the
  first run's signals), and it fails both directions without the fix.
- **A header for the linker** ([#66](https://github.com/VOTV-MP/Multivoid/pull/66)): `SteamNetworkingIPAddr::ToString`'s inline body lives in
  `isteamnetworkingutils.h`, which `session_receive.cpp` now includes instead of relying on another file to.
- **The desk's signal view** ([#81](https://github.com/VOTV-MP/Multivoid/pull/81)): the two bounds of `remapValue` and `visualizerMode` were polled by
  nobody, so each stayed on the machine that set it and a joiner got none. They now ride the desk input
  lane and the joiner's desk state (build 265), applied through the game's `updRemap` and `setCanvas`.
  Kept as written; its drill was moved onto a shared peer-signal helper that reads only its own run, and its
  joiner now waits for the host's state before reading it (a read one second early had looked like a miss).
- **A dish's slew, heard whole** ([#82](https://github.com/VOTV-MP/Multivoid/pull/82)): a client's dish is a parked copy moved by the host's poses, so it
  played one motor sound where the host's body plays a hum, the motor, a clunk per axis and a fading end. The
  client now plays that sequence from the edges in the stream it already applies. Kept as written; the hum now
  starts on the stream's moving flag, as the host's starts at the slew's order before its random wait, and the
  drill judges the client's sequence against the host's itself.
- **A save every player hears of** ([#69](https://github.com/VOTV-MP/Multivoid/pull/69)): a client's world save is blocked, so the game's SAVED indicator
  showed on the host alone. The host now hears its own world write and the game's refusal during an event,
  and every player gets one chat line per attempt (a new host-only kind, build 266). Kept as written; its
  drill now counts the notices and judges the client's against the host's and against its own steps.
- **A spill every peer sees** ([#74](https://github.com/VOTV-MP/Multivoid/pull/74)): a cup, a beer, a wine or a gas bottle spills through a projectile
  each peer flew on its own, so the stains landed apart or not at all. The host's decal, splash and pour now
  reach every peer (four kinds, build 267). Kept as written; two holes were closed on top: the host's copy of
  a prop another player holds -- a pose-driven hold or the mirror of the item in that player's hand -- no
  longer spills a second stain of its own, and a spill the host makes while no client is connected still
  reaches a client who joins later. The drill was moved onto the shared helpers, its joiner walks to the host
  first, and its rejoin leaves as a quit so the host's absent spill is measured.
- **A vent crawl every peer sees** ([#78](https://github.com/VOTV-MP/Multivoid/pull/78)): the crawler's crawl, doors and keypads already crossed,
  but what its own graph plays -- the vent steps, the crawl's steps, the metal break and burst, the
  signal room door's power and the vent's swing -- played on the host alone. The host now cues each
  peer when its box is entered, and every client plays the sequence itself. Kept as written; on top,
  a joiner is told where a crawl stands (one host-only kind, build 269) and finds the vent at the
  host's own end pose, the host's record of the crawl outlives its last client leaving, and the
  three times cite the blueprint's own delays. The drill was moved onto the shared helpers, walks
  into the box instead of teleporting, and rejoins after the crawl.
- **One holder per prop** ([#68](https://github.com/VOTV-MP/Multivoid/pull/68)): two players who grabbed one prop both streamed it, and every other
  peer ran two drives on one copy. A peer now lets its own grab go when another hold already drives the
  prop, with the deny click and a line naming the holder, and the host keeps the first hold and refuses
  a later one (one host-to-all kind, build 271). Kept as written; the two-peer grab race that
  measures it was moved onto the shared helpers: it names its target and its go instant through the
  drill signals, takes each peer's role from the session, and judges itself, allowing the refused hold
  its one round trip before the host's refusal arrives.
- **A can's lid and contents on every copy** ([#67](https://github.com/VOTV-MP/Multivoid/pull/67)): a can opened or eaten from on one machine stayed
  shut and full on the others until the next join. A can's change now publishes its record, a client's
  through the host, and a record landing is not echoed back (no wire change). Kept as written; on top,
  the lid's flag is read through its own bit, and the drill was moved onto the shared helpers: the
  client walks to the host, every step waits on the can's state, and the host judges both peers' rows.
- **A food's one author** ([#76](https://github.com/VOTV-MP/Multivoid/pull/76)): a food's tick rolled its own temperature and spoilage on every peer and
  burnt it on each. The host is now a world food's one author and a client's copy takes its values; a
  client's bite and its let-go of a held food reach the host, and a held food stays its holder's (four
  kinds, build 272). Kept as written; on top, a client's world food is refused its tick at the script
  gate instead of parked by the tick switch (which the game turned back on), a bite and a let-go are
  judged within reach and bounded, a refused one is answered with the food's own state, the let-go is
  taken at the drop instead of polled, and the drill was moved onto the shared helpers with a
  placed-down leg. The shape follows ElvardFIC's and AquaFishhh's food lane, below.
- **One set of ambient creatures** ([#83](https://github.com/VOTV-MP/Multivoid/pull/83)): each peer's spawners rolled their own deer, walking
  trees, hexahive and sus holes, and beehive branches. A client's spawners lose their tick and its
  beehive timer is paused; the host's deer and trees are mirrored by the npc lane, a tree's size and
  landing cross as one kind, and holes and branches cross by place as another (two kinds, build 273).
  Kept as written, except the host-side sus-hole roll around each client, which was not adopted; on
  top, a client's spawners are refused at the script gate, a sus hole is its player's roll made by the
  host, a branch is bound by its nest's birth key and the join ends the census, the ambient actors end
  at their end of play, a deer's mirror keeps its tick on a refreshed floor, and the drills were moved
  onto the shared helpers with a host verdict.
- **A woodchipper every peer runs the same** ([#88](https://github.com/VOTV-MP/Multivoid/pull/88)): a chipper's fuel and its on/off lived on
  whichever machine touched it, so the host's chipper, the one that eats the props, could stand off and
  empty while a client poured and pressed its own copy. The host's fuel and switch now reach every peer,
  and a client's pour and press go up as requests (two kinds, build 279). Kept as written; on top, a
  client's copy runs no tick and no intake (refused at the script gate), so only the host burns fuel,
  eats a prop and drops a clump. The host's state is sent from the game's own checkActive and button
  instead of polled, and the client's pour and press are read from the game's functions instead of a fuel
  threshold. The host judges each request (reach, rate, a living sender, the gas can in its hand, room in
  the tank, the state wanted) and answers a refusal to the sender alone. A joiner is told which chippers are
  on, and the drill was moved onto the shared helpers with a host verdict. A client cannot yet feed a prop
  to it: measured, the host's chipper did not eat a dropped paper, so that is a tracker line.
- **One state for every plant: the host's** ([#80](https://github.com/VOTV-MP/Multivoid/pull/80)): each peer's plants grew
  on their own water, sun and rain, and the winter bushes were planted on every peer. The host now runs
  every plant and sends its state; a client's player edits go up to it. Kept as written; on top, the
  host plants only the class its own plant table names and only within the sender's reach (a client
  could make it spawn any class anywhere), the family is every class under the plant base, the
  client's own tick, sprinklers, growth areas, fire and spawners are refused at the gate instead of
  parked, the growth area's glow crosses as a cue, an edit is read before a state lands on it, and a
  ripe bush keeps its berry count. The drill moved onto the shared helpers, each arm green and red.
- **A disc full of server data, and a module row bought by the host** ([#86](https://github.com/VOTV-MP/Multivoid/pull/86)): a disc of about 21
  dish rows passed the slot lane's 32 KB ceiling and the save record's, so the box stopped syncing and an
  eject left the host a blank disc; and a client's press on a module row of the upgrade panel paid on its
  own machine alone, until the host's next balance put it back. Both ceilings were raised to one blob, and
  the press became an intent the host runs on its own row (build 282). Two of the PR's three commits are
  kept as written; on top, one ceiling for a disc record, defined once and used by the sender and the
  receive (56,092 B), with a record over it refused at the receive and its size logged. A full box disc at
  32 writes measured 48,982 B and a laptop disc at 31 writes 47,460 B, both under it. The host presses
  a module row only for the laptop's holder and at a rate limit, a refused press is answered to the sender alone with the host's
  balance and levels, and the storeless rows charge their price, as row 23's 30 credits measured. The two drills were
  moved onto the shared helpers, wait on the peers' state, are judged on the host, and have a join arm and
  a red each. The third commit (two discs put in one slot end as a swap) waits: the loss it answers comes
  from the disc slots being read once a second while the game announces both acts, and a swap on top of
  that poll would extend it. The slots move onto the game's insert and eject events first, then the swap
  is re-judged and the request stays open until then. A zip disc (1024 writes) still exceeds the ceiling
  and is refused with its size logged; carrying it needs chunked records, a tracker line.
- **A toolgun's flags, the host's** ([#85](https://github.com/VOTV-MP/Multivoid/pull/85)): the toolgun's prop-properties tool sets a prop static,
  frozen or asleep on the pressing machine alone, so a client's freeze held only on its own screen. The PR
  made a press cross, and the host's copy and every other peer's followed (two new kinds). One of
  the request's four lanes is kept as written, the tool-flags piece; the other three (rest and settle,
  fire, the join sweep) come in their own packages and the request stays open. On top, the tool's own press
  is the seam instead of a watch on every class's init: a client's press is cancelled and goes up as a
  request, and the host judges it like the remover (the toolgun permission, a toolgun in hand, sight, and
  another peer's hold) and publishes the flags it applied, read back from the prop. A static prop reaches
  a joiner through the game's own setter, and classes that override it (a landmine, a TV) keep their own
  state. Measured first: the tool's press froze a prop from 0,0,0 to 0,1,0, and the dumps show only the
  tool and the stacker setting these flags. The drill has five arms (freeze, make-static with a rejoin,
  refused without the permission, refused out of sight, a landmine) and two reds.
- **Room for more message kinds** ([#117](https://github.com/VOTV-MP/Multivoid/pull/117)): the reliable lane named a message's kind in one byte, and
  its 256 values were nearly spent. The kind now takes two bytes in the header's first pad byte, so the header
  stays eight bytes and every existing kind is the same bytes on the wire. Kept as written; on top, every
  per-kind table and check covers the new space, a kind past it is said once instead of vanishing, and the
  CI drill and the kind gate read the wider enum.
- **An opened MRE's contents** ([#108](https://github.com/VOTV-MP/Multivoid/pull/108)): a client's MRE open rolled and
  spawned the contents on that client only. Now the host opens it with the MRE's own spawn, so the contents are the
  host's births on every peer. Kept as written; on top, the slot's granted verb is set and cleared with its reservation.
- **A zip disk dropped in the drone sack** ([#111](https://github.com/VOTV-MP/Multivoid/pull/111)): a client's
  disk dropped into the drone sack was filed on that client only, and the sack's call destroyed the prop,
  taking the host's copy with it unsold. Now the client's put from the sack is cancelled and the host, which
  runs the same overlap, files it. Kept as written; on top, the cancel no longer lists its callers: on a
  client every overlap put into a shared container is cancelled, the roomba's own pickup the one named
  exception, and the drone sack drill's arm 2 carries its prop over the sack and says its verdict.
- **One walking mannequin, the host's** ([#84](https://github.com/VOTV-MP/Multivoid/pull/84)): each peer's mannequin spawner rolled its own walkers and
  each copy ran its own brain, so a client's walker drifted from the host's and hunted whoever it liked. The PR froze the
  host's walker on a client's view and steered it toward the nearest player (two kinds). Kept for its authorship and for
  the hit it delivers: a walker's hit lands on the client it strikes, a ragdolled puppet is spared, and the host's copy of
  the hit sound is held back. On top, a client refuses the mannequin spawner at the script gate and its copy runs only its
  begin-play and its destroy, with a nav pawn that takes no controller; the host's walker moves its copy on the drive
  stream, claimed at its network id; a walker hunts one player and keeps it until that player leaves, dies or leaves its
  range; it freezes on its target's gaze only, the host answering the walker's own gaze reads with that client's level;
  its pose and target cross on their edges and to a joiner (two kinds, build 283); and the drills became one drill on the
  shared helpers with a host verdict, seven steps and a red arm for each mechanism.
- **One timer clear for six callers** ([#105](https://github.com/VOTV-MP/Multivoid/pull/105)): six hand-built copies of the engine's timer clear became one helper. Kept as
  written; on top, the helper resolves the engine's function on every call until it
  is found, so no caller can read another caller's failed attempt as its own.
- **A drill keeps its players fed and rested** ([#100](https://github.com/VOTV-MP/Multivoid/pull/100)): a long drill's players fainted from hunger and sleep before it ended. Kept as
  the idea; on top, it became a dev switch of the existing vitals keepalive, so each
  player refills its own food and sleep through the game's vitals, with no second
  helper beside it.
- **A joiner's empty equipment slots** ([#114](https://github.com/VOTV-MP/Multivoid/pull/114)): a joiner's empty equipment slots were written as zeroed structs the game never
  reads as free, so a player could wear nothing new. Kept as written; on top, the
  blank is read from the save object's own defaults rather than typed, and the drill
  fails only on the game's own "Not enough space" answer.
- **A knocked-down pinger frees the desk** ([#97](https://github.com/VOTV-MP/Multivoid/pull/97)): a pinger knocked down or killed at the desk kept holding it. Kept as written; on
  top, a death frees it too, through a life edge that now also fires for a player
  already dead when their copy first appears, once that player's world is ready, and
  the drill decides each arm at the host's reconcile rather than on a timer.
- **An ATV's fuel, modules, controls and horn** ([#89](https://github.com/VOTV-MP/Multivoid/pull/89)): a client's refuel,
  repair, module fit or take-off, its lights, brake and turbo, and its horn changed that
  client's copy of the ATV only, and each peer burnt its own fuel. Now a client's act is
  asked of the host, which judges it, applies it and publishes the result to every peer;
  the driver's controls and horn ride its stream, and the host takes the driver's burn only
  from a block that has seen the host's latest state. Kept as written: the host as the ATV's
  one author, the module refund of a refused fit. On top: every request is answered, a client
  holds the ATV's state until its answer comes, a request from a player still loading waits
  until they are ready, the horn is a level in the stream, and a drill with a red arm for
  each mechanism.

### ElvardFIC and AquaFishhh

From their own coop mod, Watchers of the Void: the shape of the food lane -- the host as a world food's one
author, a client's bite as an intent, and a held food's condition for the copy others show in that hand.
Ghezra's ([#76](https://github.com/VOTV-MP/Multivoid/pull/76)) rewrote it on this tree's lanes, and the food sync stands on it.

## Reports and reviews

Their full write-ups are in [credits-reports.md](credits-reports.md): the reports, and the reviews that changed a decision.

## Maintenance note

This file (with credits-reports.md) and the two short tables move together: anything that lands a row
**here** lands a line in both short tables, the README's credits table and the
website's credits section.

Kinds are `code`, `report` and `review`. A person can hold more than one — give
them one row with both, never two rows.
