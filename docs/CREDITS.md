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
**[INSTALL.md](INSTALL.md)**.

---

## The ledger

Grouped by kind; within a group, largest or most recent first. Commit counts come
from `git shortlog -sne` and fold each person's identity variants together.

| Who | Kind | Contribution | Landed |
|--|--|--|--|
| **pelmentor** | code | Architecture, direction, releases — the whole mod | 464 commits |
| **Claude** (Anthropic) | code | Implementation, across the whole mod | 1,368 commits |
| **Tarangok** | code | KO respawn, live skin preview, held-prop visibility, container extraction | 10 commits |
| **hediiiqq** | code | Dish mirror interpolation | 4 commits |
| **arigalit** | code · report | ATV seat contention ([#9](https://github.com/VOTV-MP/Multivoid/pull/9)); join-time prop-count divergence | 2 commits |
| **huoyan1231** | code · report | CI and automated builds; the b125 host-log pack | 2 commits · b134 |
| **Moddy** | review | The architecture and documentation review that became the UE4SS move | b122 · b143 |
| **SentientYeet** | review | The substrate critique that re-opened the loader decision | b143 |
| **Violet** | report | ~9 FPS for a friend joining on Linux — five separate defects behind it | b134 |
| **decodinatorX** | report | Couldn't type at the SAT console — `T` kept opening chat | b133 |
| **gediao** | report | The b125 host-log pack, with huoyan1231 | b134 |
| **SirWilliam** | report | Rejoining a session requires a full relaunch | queued |

---

## Code contributions

Community commits are adopted with their **original authorship preserved**
(`git log --author=<name>` shows exactly what each person wrote).

### Tarangok
- **KO respawn** (`death.ko_respawn`, on by default): lethal damage becomes a
  knock-out and a respawn at the КПП start instead of the stock kick-to-menu
  permadeath — the foundation of the mod's respawn system.
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

### arigalit
- **ATV seat contention** ([#9](https://github.com/VOTV-MP/Multivoid/pull/9)): a
  peer walking up to an ATV somebody else is already driving is denied at the
  input seam, instead of both engines running vehicle physics and fighting over
  the body.

### huoyan1231
- CI and automated builds (`.github/workflows`).

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

**The conclusion above stands, and it survived a challenge (2026-08-29.)** For
part of that day this row carried a correction saying the mod had been measured
at **120 → 70 fps**, roughly 6 ms/frame. That correction was wrong and has been
withdrawn. The 50 fps was real, but it was not Multivoid: it was the developer
machine's own tooling. Bisected on one save, one Multivoid build, one windowed
launch —

    dev rig as found ............................... ~75 fps
    minus DebugMod.pak (Content/Paks/LogicMods) .... ~89 fps
    minus UE4SS's bundled Lua mods (Mods/mods.txt) . ~119 fps
    Multivoid loaded, hosting, its own paks present . ~119 fps

The same machine ran the **same** build at a stable 120 through r2modman.

**WHY, THOUGH, WAS WRONG TWICE, and the second correction is the one to read.** This row first
said r2modman's profile ships no `mods.txt` at all. `[V]` FALSE: it has one, at
`shimloader\mod\mods.txt`, enabling the SAME six Lua mods, and that run's own `UE4SS.log` shows
all six `Starting Lua mod` plus `BPModLoaderMod` mounting the same `DebugMod.pak`. The census that
missed it was looking in the game folder, where the managed lane keeps nothing.

So the mods were running in the fast environment too, and the bisect above -- which is real, with a
negative arm -- proves an effect ON THAT RIG without naming its cause. The remaining difference is
the LOADER: `[V]` `ue4ss.dll` Git SHA `d935b5b` (the zDEV archive, dated 2024-02-14) on the dev rig
against `e31aaaa6` (2026-05-07) under shimloader, both self-labelled v3.0.1 Beta #0. Swapping ONLY
that file, same save, same pinned mod DLL, same window: **80 fps median -> 106**.

**NOT SETTLED, and it is not written into the install instructions for that reason:** the new
loader did not start `CheatManagerEnablerMod` (5 of 6), so the loader and that one mod are
confounded in the measurement, and the de-confounding arm has not run. Note the uncomfortable
direction this points: `docs/INSTALL.md` currently tells manual installers to use UE4SS's zDEV
archive, which is the SLOW binary here.

So Violet's remainder really was separate from the mod. Two things are worth
keeping from the detour, because they are what made a wrong answer plausible for
a day: every counter the mod owns times **its own code**, so none of them can
price the engine work that code provokes — and a comparison between two installs
is worthless until you have diffed the installs. Multivoid now ships a boot
notice naming any frame-costly mods it finds beside it, so no player has to
repeat this. Details in `docs/LESSONS.md` §7.

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
blind** (on a Russian layout the `T` key produces `е`, and the check was on the
character, not the key), and there are **two different consoles** in this game —
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
work until the game is fully restarted · **Status:** filed, not yet fixed

Filed as a session-lifecycle row from the same b125 triage map. Reproduction and
fix are queued; the row stays here, openly unfixed, rather than being quietly
dropped.

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
`FEASIBILITY.md` still announced "Chosen approach: UE4SS + reflection", a decision
reversed the day after it was written and never annotated, and the overlay was
still described as riding "UE4SS's built-in ImGui" months after the mod
hand-rolled its own present hook. That pair became a standing project lesson: in a
public repo, an un-annotated superseded decision is ammunition.

**The honest part.** The central claim was answered with a measurement — the
replaceable surface was 7,174 of 146,347 lines, about 5% — and **refused**, on the
ground that the shipping mod must not require players to install a second loader.
That refusal was published. **Four weeks later it was overturned and the mod moved
onto UE4SS anyway**; Multivoid ships today as a UE4SS mod. The argument that
carried the day was the one he had already made, and what changed was not a better
case from the other side but a re-audit that found the refusal's own premises
unsound. The record of both, including the losing answer, is kept in
`docs/VERSION_MIGRATION.md` §7 rather than quietly edited away.

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
**here** lands a line in **both** short tables (README §Credits and
`site/templates/index.html` §07). The site is deployed by hand — the built
`site/public/` must be regenerated and uploaded for a site-side change to appear.

Kinds are `code`, `report` and `review`. A person can hold more than one — give
them one row with both, never two rows.
