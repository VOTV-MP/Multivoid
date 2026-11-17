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
| [**Ghezra**](https://github.com/Ghezra) | code | A remote player's own body cut a silhouette out of its flashlight beam on every other screen ([#71](https://github.com/VOTV-MP/Multivoid/pull/71)); an empty battery left the other peers' copy of a player lit ([#79](https://github.com/VOTV-MP/Multivoid/pull/79)); a link fix for the Steam networking header ([#66](https://github.com/VOTV-MP/Multivoid/pull/66)); the desk's signal view bounds and its graph or spectrogram mode staying on one machine ([#81](https://github.com/VOTV-MP/Multivoid/pull/81)); a client's dish slewing in silence but for one motor sound ([#82](https://github.com/VOTV-MP/Multivoid/pull/82)); a client never learning that the host saved, or that a save failed ([#69](https://github.com/VOTV-MP/Multivoid/pull/69)); spills that landed apart on each peer ([#74](https://github.com/VOTV-MP/Multivoid/pull/74)); a vent crawl only its own host heard and saw ([#78](https://github.com/VOTV-MP/Multivoid/pull/78)); two players holding one prop at once ([#68](https://github.com/VOTV-MP/Multivoid/pull/68)); a can opened or eaten from on one machine only ([#67](https://github.com/VOTV-MP/Multivoid/pull/67)); a food that cooled, ripened and burnt on each peer by itself ([#76](https://github.com/VOTV-MP/Multivoid/pull/76)); deer, walking trees, dirt holes and beehive branches rolled again on every peer ([#83](https://github.com/VOTV-MP/Multivoid/pull/83)); plants that grew, ripened and died differently on each peer ([#80](https://github.com/VOTV-MP/Multivoid/pull/80)); a woodchipper whose fuel and switch each peer ran on its own ([#88](https://github.com/VOTV-MP/Multivoid/pull/88)); a disc full of server data that stopped syncing past 32 KB, and a module row a client paid for on its own machine alone ([#86](https://github.com/VOTV-MP/Multivoid/pull/86)); a toolgun's freeze, unfreeze or make-static that changed the pressing machine's copy only ([#85](https://github.com/VOTV-MP/Multivoid/pull/85)); a walking mannequin every peer rolled and drove by itself, and that froze on anyone's gaze ([#84](https://github.com/VOTV-MP/Multivoid/pull/84)); the message kind's one byte nearly spent ([#117](https://github.com/VOTV-MP/Multivoid/pull/117)); an MRE a client opened that rolled its contents on that client alone ([#108](https://github.com/VOTV-MP/Multivoid/pull/108)); a zip disk a client dropped in the drone sack, filed on that client only while the host copy was lost unsold ([#111](https://github.com/VOTV-MP/Multivoid/pull/111)); six copies of the timer clear made one ([#105](https://github.com/VOTV-MP/Multivoid/pull/105)); a drill's players kept fed and rested ([#100](https://github.com/VOTV-MP/Multivoid/pull/100)); empty equipment slots a joiner could never fill ([#114](https://github.com/VOTV-MP/Multivoid/pull/114)); a knocked-down pinger that kept the desk ([#97](https://github.com/VOTV-MP/Multivoid/pull/97)); an ATV's fuel, repairs, modules, lights, brake, turbo and horn that each peer changed on its own copy ([#89](https://github.com/VOTV-MP/Multivoid/pull/89)); a first aid kit a client unpacked that rolled its contents on that client alone ([#119](https://github.com/VOTV-MP/Multivoid/pull/119)); the hovering drone holds still on a client ([#96](https://github.com/VOTV-MP/Multivoid/pull/96)); the pause menu's leave during an event ([#124](https://github.com/VOTV-MP/Multivoid/pull/124)); the base computer's short circuit ([#95](https://github.com/VOTV-MP/Multivoid/pull/95)); the sky teleport's, a faucet's and the alien sounds ([#104](https://github.com/VOTV-MP/Multivoid/pull/104)); a client's own nightmare ([#125](https://github.com/VOTV-MP/Multivoid/pull/125)); the cargo lift is the host's ([#118](https://github.com/VOTV-MP/Multivoid/pull/118)); an anchored hook whose client copy had its collision off, so a client could not take its own hook back ([#112](https://github.com/VOTV-MP/Multivoid/pull/112)); a nametag put on a prop that showed only on the peer that clicked it ([#113](https://github.com/VOTV-MP/Multivoid/pull/113)); the refiner's cap label on every peer ([#106](https://github.com/VOTV-MP/Multivoid/pull/106)); a backrooms trip that is one player's ([#126](https://github.com/VOTV-MP/Multivoid/pull/126)); a fuse or an upgrade a client installs from its hand that the host refused ([#110](https://github.com/VOTV-MP/Multivoid/pull/110)); the black orb ([#127](https://github.com/VOTV-MP/Multivoid/pull/127)), the ambient UFOs ([#128](https://github.com/VOTV-MP/Multivoid/pull/128)), the earth teleport's sky ([#129](https://github.com/VOTV-MP/Multivoid/pull/129)) and the robot cleaner ([#130](https://github.com/VOTV-MP/Multivoid/pull/130)), which a client saw only after a rejoin; a selftest that ran every tick ([#163](https://github.com/VOTV-MP/Multivoid/pull/163)); the log's date and UTC offset ([#164](https://github.com/VOTV-MP/Multivoid/pull/164)); a remote ATV driver that did not sit on the ATV on the other screens, and the host's kerfur that did not ride along on every peer ([#92](https://github.com/VOTV-MP/Multivoid/pull/92)); containers that break, nest and carry photos, a garbage container's contents, and a hacksaw's cut ([#93](https://github.com/VOTV-MP/Multivoid/pull/93), [#94](https://github.com/VOTV-MP/Multivoid/pull/94)); an ATV battery a client fitted or unscrewed that changed its own copy only ([#140](https://github.com/VOTV-MP/Multivoid/pull/140)); a dropped can that came back closed and full, and a held item that showed its default state on other screens ([#136](https://github.com/VOTV-MP/Multivoid/pull/136)); a glowstick a client stuck to a wall or a prop that hung free on every other screen ([#162](https://github.com/VOTV-MP/Multivoid/pull/162)); sleep that sped the world up for players still awake ([#142](https://github.com/VOTV-MP/Multivoid/pull/142)); the crowbar a client's pry hands back, lost for everyone else ([#161](https://github.com/VOTV-MP/Multivoid/pull/161)); a prop a player walked into that came to rest in a different place on each screen ([#99](https://github.com/VOTV-MP/Multivoid/pull/99)) | 70 commits · unreleased |
| **Moddy** | review · design | The architecture and documentation review that became the UE4SS move; the public UE-Modding-Tools pointer that became the blueprint-CFG rung and the migration scanner (patternsleuth); and design published for [Relay](https://github.com/modestimpala/Relay), Moddy's Blueprint networking API for VOTV ([Thunderstore](https://thunderstore.io/c/voices-of-the-void/p/Moddy/Relay/)), in its README and the [README Blueprint](https://blueprintue.com/blueprint/g3s09x9c/) that README links: the watch surface on a Blueprint function, a watch that reads the parameters before the call and can cancel it, and one that only observes after it, which became the script-body gate; the readable join reason and stable diagnostic codes, which became the join screen's named steps and the end-reason codes; the list of cheap edge protections, of which two were missing here: a per-source limit on connections and a private access list on the identity key file; the rule columns that make an actor's own save record its spawn payload, the general form of what this project's prop save-data work was building case by case; the client-only `Quiesce` column, which made this project state its parking rule once and read every park against it; the one paragraph on container handling, which made it write down and measure its own container invariants; the note that a watch on a parent class misses a child's override, which sent us to audit every hook we install, which found three seams that had never installed and two verbs called on the wrong class; and the list of what a returning player's profile covers, which is the list this project's per-player profile now carries | b122 · b143 · 2026-09-02 · b153 · b157 · b160 · b161 · b167 |
| **SentientYeet** | review | The substrate critique that re-opened the loader decision | b143 |
| **Violet** | report | ~9 FPS for a friend joining on Linux — five separate defects behind it | b134 |
| **decodinatorX** | report | Couldn't type at the SAT console — `T` kept opening chat | b133 |
| **gediao** | report | The b125 host-log pack, with huoyan1231 | b134 |
| **doctaaaaa** | report | A ten-item field pack on the released build, of which four landed on open work: a floppy disc lost when it is retrieved from a signal server, a recorded signal lost when the disc changes hands, the power chain, and the trash-pile cost. The disc pair is now root-caused — the box on the other machine takes the disc back the moment it appears | b150 · root-caused, unreleased |
| **SirWilliam** | report | Rejoining a session requires a full relaunch | fixed, unreleased |
| **thewittyrobin** | report | Doors that never open for the other peer, twice ([#17](https://github.com/VOTV-MP/Multivoid/pull/17), [#18](https://github.com/VOTV-MP/Multivoid/pull/18)). Both proposed mechanisms measured out differently, and both reports are of one real defect: on the public build a door is addressed by a key the game re-mints per process | root-caused, unreleased |
| [**ElvardFIC**](https://github.com/ElvardFIC) and [**AquaFishhh**](https://github.com/AquaFishhh) | code | The shape of the food lane, from Watchers of the Void: the host as a food's one author, a client's bite as an intent, a held food's condition for the copy others show | food lane |
| [**petmav**](https://github.com/petmav) | code | The stutter a joiner felt once its loading screen dropped ([#157](https://github.com/VOTV-MP/Multivoid/pull/157)); why a saved signal or a deck selection was refused, said in the log, a signal row whose names two peers spell in different case, and a traffic census by message kind ([#158](https://github.com/VOTV-MP/Multivoid/pull/158)); the player cap raised past four, with the master's lobby seats to match ([#152](https://github.com/VOTV-MP/Multivoid/pull/152)); slower hunger in larger groups, a host rule ([#154](https://github.com/VOTV-MP/Multivoid/pull/154)); host and player cheat panels in the F1 menu ([#146](https://github.com/VOTV-MP/Multivoid/pull/146)); a dropped prop that settled differently on each screen, and an extinguisher that ran away on one only ([#156](https://github.com/VOTV-MP/Multivoid/pull/156)) | 12 commits · unreleased |

---

## Code contributions

Community commits are adopted with their **original authorship preserved**
(`git log --author=<name>` shows exactly what each person wrote).

What each person's code turned out to be, and what shipped from it, is on that person's page:

- [Tarangok](credits-code/tarangok.md)
- [hediiiqq](credits-code/hediiiqq.md)
- [arigalit](credits-code/arigalit.md)
- [Marlore](credits-code/marlore.md)
- [huoyan1231](credits-code/huoyan1231.md)
- [Wigard](credits-code/wigard.md)
- [Shinobu2](credits-code/shinobu2.md)
- [Slintchen](credits-code/slintchen.md)
- [archhn0madd](credits-code/archhn0madd.md)
- [AwazonDotCom](credits-code/awazondotcom.md)
- [gust138](credits-code/gust138.md)
- [Ghezra](credits-code/ghezra.md)
- [ElvardFIC and AquaFishhh](credits-code/elvardfic-and-aquafishhh.md)
- [petmav](credits-code/petmav.md)

## Reports and reviews

Their full write-ups are in [credits-reports.md](credits-reports.md): the reports, and the reviews that changed a decision.

## Maintenance note

This file (with credits-reports.md and the pages in credits-code/) and the two short tables move
together: anything that lands a row **here** lands a line in both short tables, the README's credits
table and the website's credits section. A code contributor's new entry goes at the end of their page
in credits-code/; a new contributor gets a page and a line in the index above.

Kinds are `code`, `report` and `review`. A person can hold more than one — give
them one row with both, never two rows.
