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
public, that a decision in it was wrong.

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
| **arigalit** | code · report | ATV seat contention ([#9](https://github.com/VOTV-MP/Multivoid/pull/9)); join-time prop-count divergence; the grappling-hook lane ([#16](https://github.com/VOTV-MP/Multivoid/pull/16)) — four of its decisions are in the shipped lane | 2 commits · 2 co-authored |
| **huoyan1231** | code · report | CI and automated builds; the b125 host-log pack | 2 commits · b134 |
| **Marlore** | code · report | A nine-symptom pass over the trash carry ([#29](https://github.com/VOTV-MP/Multivoid/pull/29)). Four were real defects and are fixed at the root, co-authored: a refused grab that was never answered, a pile that looked different on every peer (865 of 871 measured), a thrower refused every later grab, and a fallen player who kept carrying | 6 commits · unreleased |
| [**Wigard**](https://github.com/wigarddev) | code | A guest of a sandbox host came up in story ([#32](https://github.com/VOTV-MP/Multivoid/pull/32)) — the transfer header had carried the host's game mode from the start and the host filled it with a constant. Then seven more, each measured red and green before it was sent: a build that did not link on VS 2022 ([#30](https://github.com/VOTV-MP/Multivoid/pull/30)), the shower's mirrored toggle running the wrong verb ([#31](https://github.com/VOTV-MP/Multivoid/pull/31)), a drive left frozen in the port on the far peer ([#33](https://github.com/VOTV-MP/Multivoid/pull/33)), a pile a client bags that the host never learns of ([#34](https://github.com/VOTV-MP/Multivoid/pull/34)), a client's upgrade purchase that charged nobody ([#35](https://github.com/VOTV-MP/Multivoid/pull/35)), a prop whose record changes in place telling no one ([#36](https://github.com/VOTV-MP/Multivoid/pull/36)), the bay window's sponge dabs reaching no other peer ([#37](https://github.com/VOTV-MP/Multivoid/pull/37)), and a client's press of the drone console reaching a drone that cannot fly ([#38](https://github.com/VOTV-MP/Multivoid/pull/38)). Then a disc the host ejects that reached the others blank, with the disc drill that proves it ([#39](https://github.com/VOTV-MP/Multivoid/pull/39)), and a desk repaint whose diagnosis matched a fix main already carried ([#40](https://github.com/VOTV-MP/Multivoid/pull/40)) | 13 commits · unreleased |
| [**Shinobu2**](https://github.com/Shinobu2) | code | About twenty lanes in one request ([#45](https://github.com/VOTV-MP/Multivoid/pull/45)), adopted lane by lane: a newer save record replacing a parked one, a thrown container that kept its contents, damage and fire refused on a peer's puppet with its flame shown, the story-event creatures mirrored, an event fire crossing at the verb it ran in, a client's point sack paid by the host, the server repair's reward, the runtime ATV spawn, a joiner's daily task and nine more | 23 commits · unreleased |
| [**Slintchen**](https://github.com/Slintchen) | code | The request that the mod's UI speak the player's language ([#41](https://github.com/VOTV-MP/Multivoid/pull/41)): its Chinese translations, its system-face merge for CJK and its translator's guide carry into the translation layer being built | in progress |
| [**archhn0madd**](https://github.com/archhn0madd) | code | Rejoin without a relaunch — the boot poll answered from the dying world | 1 commit |
| **Moddy** | review · design | The architecture and documentation review that became the UE4SS move; the public UE-Modding-Tools pointer that became the blueprint-CFG rung and the migration scanner (patternsleuth); and design published for [Relay](https://github.com/modestimpala/Relay), Moddy's Blueprint networking API for VOTV ([Thunderstore](https://thunderstore.io/c/voices-of-the-void/p/Moddy/Relay/)), in its README and the [README Blueprint](https://blueprintue.com/blueprint/g3s09x9c/) that README links: the watch surface on a Blueprint function, a watch that reads the parameters before the call and can cancel it, and one that only observes after it, which became the script-body gate; the readable join reason and stable diagnostic codes, which became the join screen's named steps and the end-reason codes; the list of cheap edge protections, of which two were missing here: a per-source limit on connections and a private access list on the identity key file; the rule columns that make an actor's own save record its spawn payload, the general form of what this project's prop save-data work was building case by case; the client-only `Quiesce` column, which made this project state its parking rule once and read every park against it; the one paragraph on container handling, which made it write down and measure its own container invariants; the note that a watch on a parent class misses a child's override, which sent us to audit every hook we install, which found three seams that had never installed and two verbs called on the wrong class; and the list of what a returning player's profile covers, which is the list this project's per-player profile now carries | b122 · b143 · 2026-09-02 · b153 · b157 · b160 · b161 · b167 |
| **SentientYeet** | review | The substrate critique that re-opened the loader decision | b143 |
| **Violet** | report | ~9 FPS for a friend joining on Linux — five separate defects behind it | b134 |
| **decodinatorX** | report | Couldn't type at the SAT console — `T` kept opening chat | b133 |
| **gediao** | report | The b125 host-log pack, with huoyan1231 | b134 |
| **doctaaaaa** | report | A ten-item field pack on the released build, of which four landed on open work: a floppy disc lost when it is retrieved from a signal server, a recorded signal lost when the disc changes hands, the power chain, and the trash-pile cost. The disc pair is now root-caused — the box on the other machine takes the disc back the moment it appears | b150 · root-caused, unreleased |
| **SirWilliam** | report | Rejoining a session requires a full relaunch | fixed, unreleased |
| **thewittyrobin** | report | Doors that never open for the other peer, twice ([#17](https://github.com/VOTV-MP/Multivoid/pull/17), [#18](https://github.com/VOTV-MP/Multivoid/pull/18)). Both proposed mechanisms measured out differently, and both reports are of one real defect: on the public build a door is addressed by a key the game re-mints per process | root-caused, unreleased |

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
  so CJK text renders, and a guide for translators. The layer is being rebuilt on gettext catalogues with
  contexts and plural forms, and with only the text a player reads wrapped (the request had also wrapped
  engine names a reflected call looks up); the translations, the face merge and the guide carry into it under
  Slintchen's name.

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

---

## Reports

### Violet — ~9 FPS for a friend joining on Linux

**Channel:** Discord · **Reported:** a friend joining her session ran the game at
about 9 FPS on Linux (Proton) · **Shipped in:** b134

**What it turned out to be.** Five separate defects, none of them the one the
symptom pointed at. The triage of her log found:

1. **A dead world's actors were still being used.** After any quit-to-menu, the
   mod kept handing actors belonging to the destroyed world back into engine
   calls. The engine faulted on each one — about **2,500 absorbed access
   violations per second**, every one of them written to the log. The root: a
   dying world's actors are not marked dead until garbage collection runs, which
   was measured at **44+ seconds** later, so "is this object alive?" was
   answering yes for a world that no longer existed. Every cached engine
   reference now carries the world it came from and is dropped when that world
   goes.
2. **~1,600 spurious destroy broadcasts on every client world load** — from
   **two independent causes**, which is why an earlier partial fix had not
   closed it. One was silenced at its source; the other was structural: the quiet
   period meant to cover the world reload was closed by the very latch that
   starts the rebuild, so by construction it always ended before the work it was
   protecting.
3. **A once-per-second stutter everyone could feel.** Thirteen separate
   subsystems each walked the engine's entire object array on their own
   schedule. They now share one budgeted scan.
4. **A periodic freeze.** A full object-array census could stall a single frame
   for nearly two seconds on her friend's machine. It now runs spread across many
   frames, capped at about 1 ms each.
5. **A silent reliable-message drop under load** — see the huoyan1231 + gediao
   row below, which the same work closed.

**The honest part.** After all five, her friend's frame rate was still low, and
that remainder was measured — at the time — not to be the mod: Multivoid's own
per-frame cost came out under a millisecond. The report was still worth every
hour; none of the five would have been found without it.

**The remainder was measured again and it was still not the mod.** The first answer -- that a
debug pak and the loader's bundled Lua mods cost the frames -- did not survive a controlled
follow-up. Changing nothing but `UE4SS.dll` took one machine from about 70 to about 118 fps,
while disabling a Lua mod on the old build was worth about 5, so the frames belong to the
**loader build**, and Multivoid now pins the fast one for you. Two things are worth keeping from
that detour, because they are what made a wrong answer plausible for a day: every counter the mod
owns times **its own code**, so none of them can price the engine work that code provokes, and a
comparison between two installs is worthless until you have diffed the installs.

---

### decodinatorX — could not type at the SAT console

**Channel:** [GitHub issue #5](https://github.com/VOTV-MP/Multivoid/issues/5) ·
**Reported:** typing `sv.request` at the in-game SAT terminal was impossible —
pressing `T` opened Multivoid's chat instead · **Shipped in:** b133

**What it turned out to be.** The mod took the `T` key globally and had no way to
ask "is the game currently taking text input?" The obvious check does not work:
asking a live on-screen text box whether it has keyboard focus returns *false*
even immediately after the engine focuses it, because the widget being tested is
a cached wrapper rather than the widget the player is typing into. The working
question turned out to be asking the owning **user widget** instead.

**What shipped.** The mod stops taking keys whenever the game is typing — the SAT
console, the notepad, save-slot names, the settings search. Function keys still
reach the mod, since the game does not use them for text.

Two things worth recording from this one: the swallow was **keyboard-layout
blind** (on a Russian layout the `T` key produces the Cyrillic letter that looks like a
Latin `e`, and the check was on the character, not the key), and there are **two different consoles** in this game —
the developer console UE4 ships, and the in-world SAT terminal the report was
actually about.

---

### huoyan1231 + gediao — a full host-log pack from a real b125 session

**Channel:** Discord · **Reported:** lost props, stuck grabs and several other
oddities, delivered as a complete host log from a real session ·
**Shipped in:** b134 (headline row; other rows from the same map still open)

**What it turned out to be.** The log became a **ten-row root-cause triage map**
— the single most productive report the project has received, because a full log
from a real session shows the things nobody thinks to describe.

**The headliner:** a silent message-loss class in the reliable send path. The
host had never had the backpressure check the client had, so under load it would
**quietly drop reliable messages** — the exact shape that produces "the prop was
there for me and not for him" with nothing in any log to explain it. The rework
that closed it made delivery total: a reliable message goes into the stream, or
into the backlog, or the connection closes. It is never silently dropped.

That defect is also a lesson the project now applies generally: **a protection
added to one role only is a defect in the other role wearing a different name.**

---

### SirWilliam — rejoining requires a full game relaunch

**Channel:** Discord · **Reported:** after leaving a session, rejoining does not
work until the game is fully restarted · **Status:** fixed, unreleased —
`0288ff88`, by **archhn0madd** (see the code section above)

Filed as a session-lifecycle row from the same b125 triage map, and it sat here
openly unfixed for long enough that someone else fixed it: archhn0madd forked the
repo, rooted it, and pushed the fix on their own fork.

The root was one the project had already written down and then failed to apply
here. A dying world's actors are not kill-flagged until the GC purge — measured at
44+ seconds — which is the entire reason `world_identity` exists. But the boot
poll predated it and still asked `FindObjectByClass` directly, so after a
quit-to-menu it found the previous session's ragdolled corpse and concluded the
player was in gameplay, and found the dying world's `untitled` name and concluded
the map was already loading. The join then "succeeded" into a world that had never
loaded: `ClientWorldReady` was never announced, the host never streamed, and the
only way out was the relaunch SirWilliam reported.

The report was worth more than its two lines suggest: the same two lies also
stranded a **host** trying to re-host after a death-flee, which nobody had
reported.

---

## Review

Neither of these was a bug report. Both were people looking at how the mod is
built and saying, in public and in good faith, that a decision in it was wrong.
Both were right, and the project's largest single change came out of them.

### Moddy — the architecture and documentation review that became the UE4SS move

**Channel:** Discord, VOTV community · **Reviewed:** 2026-07-26 ·
**Landed in:** b122 (same-day documentation fixes), b143 (the substrate move)

Author of the VOTV mods `Moddy-CrashContext`, `Moddy-PBMovement` and Relay. Moddy's review
put five things to the project at once — the size of what one person plus AI was
claiming to own, what happens when the version signatures break, whether it still
works at month 18, whether **VoidTogether deserved credit**, and the central one:
*"switch to UE4SS, it does 99% of what you're doing, and it is maintained by a
team."*

**What it produced immediately.** The VoidTogether credit was agreed and shipped
the same day — the prior-art row in this project's credits exists because Moddy asked
for it. Two stale documentation claims were found and fixed the same day too:
`feasibility.md` still announced "Chosen approach: UE4SS + reflection", a decision
reversed the day after it was written and never annotated, and the overlay was
still described as riding "UE4SS's built-in ImGui" months after the mod
hand-rolled its own present hook. That pair became a standing project lesson: in a
public repo, an un-annotated superseded decision is ammunition.

**Relay.** Moddy is also the author of [Relay](https://github.com/modestimpala/Relay), a Blueprint
networking API for Voices of the Void
([Thunderstore](https://thunderstore.io/c/voices-of-the-void/p/Moddy/Relay/)). Relay ships as a
compiled mod; its README, and the [README Blueprint](https://blueprintue.com/blueprint/g3s09x9c/) that
README links, publish a good deal of its design. Everything in the eight paragraphs below was taken
from those two public pages and is used with attribution, which is what Moddy asks for what those
pages state. No code or asset of Relay's is in this repository, and every mechanism named below is
this project's own.

**The script-body gate (b160).** *Source: the README, "Watch and integration API".* Relay's README
describes a Watch on a Blueprint function: a pre phase that reads the call's parameters, a
cancelable form that can stop the call, a post phase that only observes, and the calling Blueprint frame handed to the
handler. Before this, a call one Blueprint made to another was observe-only here: the seam sat at
the call site and saw neither the arguments nor a way to refuse. Multivoid now detours the one
engine function every Blueprint body runs through, the VM's script loop, and offers that surface
(`ue_wrap/core/script_gate`): a watch by function or by name, a pre callback with the instance, the
parameters and the calling frame, a verdict, a post callback. The README says Relay's cancellation
rewrites Blueprint bytecode; this gate leaves the bytecode alone and refuses the body at the loop.
The surface is Moddy's; the seam and its derivation are this project's own.

**The join reason and the codes (b160).** *Source: the README, "Session events and state" and
"Diagnostics".* Relay's README publishes two rules for a networking layer: show the reason a world
fence is closed rather than a generic "connecting", and hand a player stable diagnostic codes to
quote. Multivoid's join screen now names the step it is waiting in and the seconds spent there, and
every join failure or disconnect carries a code (`coop/net/end_reason`, listed in `docs/join.md`)
the host and the joiner both log. The shape of the code set follows MTA's per-site literals; the
rule to publish one at all is Moddy's.

**The edge protections (b160).** *Source: the README, its section on how Relay protects network
traffic.* Relay's README lists the cheap things a host's listen edge should do before any expensive
work: check a stateless source cookie, put deadlines on the handshake, limit what one source may
open and hold pending, and write the identity seed file under a restrictive Windows access list.
Read against this project, two were already the transport's or ours (GameNetworkingSockets' connect
challenge; the pending band's deadline) and two were not. A per-source limit on connections now
runs at the accept edge, in the shape of MTA's join-flood history (`coop/net/connect_history`,
`MV-H29`), and the identity key file beside the game now carries a private access list, with an
account that cannot read it keeping its own beside it (`coop/net/peer_identity`). The list is
Moddy's; the shapes are MTA's and the project's own.

**The save record as the spawn payload (b153, b157).** *Source: the README Blueprint, the rule
profile's `spawn_payload_capture`, `spawn_payload_apply` and `identity_ready_on` columns.* Relay's
published rules declare, per class, that a spawned actor's payload is captured by the game's own
`getData` and applied by its `loadData`, and that an identity may become readable only once
`loadData` has run. That is the general form of what this project's prop save-data work was
building case by case. A prop's save data now crosses as the game's own record
(`coop/props/prop_save_data`, on `ue_wrap/actors/save_record`), and a grappling hook's record is the
payload that hands the hook over between peers (`coop/items/hook_anchor`). The declaration is
Moddy's published design; the codec, the chunking and the lanes are this project's own.

**The parking rule (b160).** *Source: the README Blueprint, the `Quiesce` field of a rule
definition.* Relay's rule definitions carry a `Quiesce` column: fields written on clients only, so
that a client's own copy of a system never makes its roll. This project already parked receivers
lane by lane; the column made it state the rule once -- a parking is the fix when it removes a
second author, and a crutch when it hides a state that was never replicated
(`docs/coop-sync-doctrine.md`, step 4) -- and read every cancel, park and neuter the mod installs
against it. The column is Moddy's published design; the rule's wording, its test and the census are
this project's own.

**The container invariants (b160).** *Source: the README, "Built-in VotV adapters" and the
traffic-protection section.* Relay's README gives its container handling one paragraph -- transfers
routed through the host, the client's rows a mirror and not the canonical copy -- and says the host
validates replicated actions, without listing the checks. There was no list to copy, so the row was
built the other way round: this project wrote down the invariants its own container lane must hold,
measured each, and closed the gaps the measuring found (`coop/props/container_write_policy` and its
siblings). The prompt is Moddy's; the invariants, the measurements and the fixes are this project's
own.

**The hook audit (b161).** *Source: the README Blueprint, the note "3 // WATCH VANILLA
BLUEPRINTS".* That note states, plainly, that a watch registered on a parent class misses a child
that overrides the function. That sentence is worth more here than its own mechanism: it sent us to
read every hook registration this mod installs against the cooked Blueprint tree, and the audit
found three seams that had silently never installed -- including a client-side cancel whose absence
let a client delete props the host still held -- one guard registered on a route no dispatch takes,
and two verbs resolved on a base class and called on subclasses that override them. None of those
had a symptom anyone had reported. The warning is Moddy's; the census, the instrument and the fixes
are this project's own.

**The per-player profile (b167).** *Source: the README, "Per-player inventory and equipment" and
"Player save partition and profile".* Relay's README says a returning player "is bound to the same
host/world profile even when their session PeerId changes", and that the profile "covers the
supported vitals, inventory, equipment, held item, effects, and player transform". This project
already stored a player's items on the host under the identity the player proved; what it did not
have was the rest of that list, and players said so in the field: a joiner started at the host's
hunger and sleep, and woke up where the host stood. The profile now carries the vitals and the
last spot the player was standing on beside the items (`coop/items/player_profile.h`); effects are
not built. Two things differ on purpose. Relay routes a client's inventory actions through the
host and calls the client's rows "a mirror, not the canonical copy"; here a player's own store is
live on their machine and streamed to the host, which keeps it. And the README does not say where
the partition is stored; this project keeps it beside the host's save, not inside it. One thing
is the same and is not taken from it: the README says Relay's overlay "runs before travel", and
this project has written a joiner's items into the save object before the world exists since
June 2026; that README was first published in September 2026. The list is
Moddy's published design; finding where the game keeps each part (what a player carries is a slot
of the save object, the vitals live in place on it, and the game moves a joiner to the host's
saved position), the write before the world exists, the fresh keys for copied items and the
storage are this project's own.

**The object index, and a correction (b160).** An earlier version of this page credited Relay's
README for the fact that the engine reports every object's creation and destruction to registered
listeners. It should not have: the mechanism is Unreal Engine's own -- `FUObjectArray` keeps a
list of create listeners and a list of delete listeners -- and nothing Relay publishes describes it.
This project came to it by examining Relay's closed DLL, by looking at which engine facilities it
links against, in work since withdrawn at Moddy's request. The engine mechanism stays, with the
pointers Moddy asked for: UE4SS, which is open source, registers on both lists in
[`LiveView.cpp`](https://github.com/UE4SS-RE/RE-UE4SS/blob/7f7cc36f8cdc082566cd676acc26975a22a41aaa/UE4SS/src/GUI/LiveView.cpp#L809-L810)
and on the delete list in
[`LuaMod.cpp`](https://github.com/UE4SS-RE/RE-UE4SS/blob/7f7cc36f8cdc082566cd676acc26975a22a41aaa/UE4SS/src/Mod/LuaMod.cpp#L5728).
Multivoid's object index (`ue_wrap/core/object_index`, on `ue_wrap/core/uobject_listeners`) appends
to those lists directly, with the member layout from RE-UE4SS, and the shared discovery pass reads
the index instead of walking the object array.

**The honest part.** The central claim was answered with a measurement — the
replaceable surface was 7,174 of 146,347 lines, about 5% — and **refused**, on the
ground that the shipping mod must not require players to install a second loader.
That refusal was published. **Four weeks later it was overturned and the mod moved
onto UE4SS anyway**; Multivoid ships today as a UE4SS mod. The argument that
carried the day was the one Moddy had already made, and what changed was not a better
case from the other side but a re-audit that found the refusal's own premises
unsound. The record of both, including the losing answer, is kept in
`docs/versioning.md` rather than quietly edited away.

### SentientYeet — the substrate critique that re-opened the loader decision

**Channel:** public, VOTV developer · **Reviewed:** 2026-08-21 ·
**Landed in:** b143

A public critique of standalone-loader mods by one of the game's own developers.
The project keeps a list of conditions that would force the loader decision to be
re-opened; this fired one that was **not on the list** — it had anticipated a
successor fork taking over, and never "the game's developers reject the approach".
The re-audit that followed broke the standing decision's fact base twice and ended
in the move to UE4SS.

The two reviews are the same argument four weeks apart. Moddy made it first and
was refuted; SentientYeet's is what re-opened it. Both names belong on it.

---

## Maintenance note

This file and the two short tables move together: anything that lands a row
**here** lands a line in both short tables, the README's credits table and the
website's credits section.

Kinds are `code`, `report` and `review`. A person can hold more than one — give
them one row with both, never two rows.
