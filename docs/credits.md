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
| [**archhn0madd**](https://github.com/archhn0madd) | code | Rejoin without a relaunch — the boot poll answered from the dying world | 1 commit |
| **Moddy** | review · design | The architecture and documentation review that became the UE4SS move; the public UE-Modding-Tools pointer that became the blueprint-CFG rung and the migration scanner (patternsleuth); Relay's published design note that the engine reports every object's creation and deletion to a listener, which became the object index; Relay's readable join reason and stable diagnostic codes, which became the join screen's named steps and the end-reason codes; Relay's list of cheap edge protections, of which two were missing here: a per-source limit on connections (a rate cap in MTA's shape, where Relay's is a pending cap) and a private access list on the identity key file; Relay's watch surface on a Blueprint function, a pre callback that reads the parameters and can cancel the call and a post callback that reads the result, which became the script-body gate | b122 · b143 · 2026-09-02 · b160 |
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

### huoyan1231
- CI and automated builds (`.github/workflows`).

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

Author of the VOTV mods `Moddy-CrashContext` and `Moddy-PBMovement`. His review
put five things to the project at once — the size of what one person plus AI was
claiming to own, what happens when the version signatures break, whether it still
works at month 18, whether **VoidTogether deserved credit**, and the central one:
*"switch to UE4SS, it does 99% of what you're doing, and it is maintained by a
team."*

**What it produced immediately.** The VoidTogether credit was agreed and shipped
the same day — the prior-art row in this project's credits exists because he asked
for it. Two stale documentation claims were found and fixed the same day too:
`feasibility.md` still announced "Chosen approach: UE4SS + reflection", a decision
reversed the day after it was written and never annotated, and the overlay was
still described as riding "UE4SS's built-in ImGui" months after the mod
hand-rolled its own present hook. That pair became a standing project lesson: in a
public repo, an un-annotated superseded decision is ammunition.

**The listener seam (b160).** His Relay project's README states, as a design fact, that the
engine reports every object's creation and destruction to a registered listener, so a
networking layer need not go looking for objects. Multivoid's object index
(`ue_wrap/core/object_index`, on `ue_wrap/core/uobject_listeners`) rides that seam, with the
member layout from RE-UE4SS, and the shared discovery pass reads the index instead of walking
the object array. The idea is his published design; the mechanism is this project's own.

**The join reason and the codes (b160).** Relay's README publishes two rules for a networking
layer: show the reason a world fence is closed rather than a generic "connecting", and hand a
player stable diagnostic codes to quote. Multivoid's join screen now names the step it is
waiting in and the seconds spent there, and every join failure or disconnect carries a code
(`coop/net/end_reason`, listed in `docs/join.md`) the host and the joiner both log. The shape
of the code set follows MTA's per-site literals; the rule to publish one at all is his.

**The edge protections (b160).** Relay's README lists the cheap things a host's listen edge
should do before any expensive work: check a stateless source cookie, put a deadline on the
handshake, cap what one source may have pending, and write the identity seed file under a
restrictive Windows access list. Read against this project, two were already the transport's or
ours (GameNetworkingSockets' connect challenge; the pending band's deadline) and two were not.
A per-source limit on connections now runs at the accept edge, as a rate cap in MTA's
join-flood shape rather than Relay's pending cap (`coop/net/connect_history`, `MV-H29`), and the
identity key file beside the game now carries a private access list, with an account that
cannot own it keeping its own under its profile (`coop/net/peer_identity`). The list is his;
the shapes are MTA's and the project's own.

**The script-body gate (b160).** Relay's README describes a watch on a Blueprint function
that fires before the call with the parameters readable and rewritable, can cancel the call, can
be scoped to one instance, and fires again after it; he offered its bytecode marker to this
project himself, unprompted, as the part worth taking. Before this, a call one Blueprint made to
another was observe-only here: the seam sat at the call site and saw neither the arguments nor a
way to refuse. Multivoid now detours the one engine function every Blueprint body runs through,
the VM's script loop, and offers that surface (`ue_wrap/core/script_gate`): a watch by function or
by name, a pre callback with the instance, the parameters and the calling frame, a verdict, a
post callback. Relay implements its watch by rewriting the function's bytecode and marking it
with a no-op jump; this mod, already owning a detour on the engine, took the surface and not the
marker. The surface is his; the seam and its derivation are this project's own.

**The honest part.** The central claim was answered with a measurement — the
replaceable surface was 7,174 of 146,347 lines, about 5% — and **refused**, on the
ground that the shipping mod must not require players to install a second loader.
That refusal was published. **Four weeks later it was overturned and the mod moved
onto UE4SS anyway**; Multivoid ships today as a UE4SS mod. The argument that
carried the day was the one he had already made, and what changed was not a better
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
