# Field reports — the full ledger

Multivoid has no QA department. It is tested in the field by the people playing
it, and their reports drive real fixes. This file is the **complete record**: who
reported, what they reported, what it actually turned out to be, and what shipped
because of it.

The [README](../README.md#field-reports) and the
[website](https://multivoid.dev/#field-reports) carry a short version of this —
every tester, one line each. The detail lives here so those stay readable as the
list grows.

**How a report gets here:** report anything on
[Discord](https://discord.gg/bA6tGBvGMN) or in
[GitHub issues](https://github.com/VOTV-MP/Multivoid/issues). If it changes the
mod, it gets a row — no gatekeeping on how polished the report is. A log pack
with "it felt wrong around here" has repeatedly been worth more than a tidy
description. What to attach: **[INSTALL.md](INSTALL.md)**.

Newest first.

---

## Violet — ~9 FPS for a friend joining on Linux

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
that remainder was **measured not to be the mod**: Multivoid's entire per-frame
cost came out under a millisecond. The report was still worth every hour — none
of the five would have been found without it.

---

## decodinatorX — could not type at the SAT console

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

## huoyan1231 + gediao — a full host-log pack from a real b125 session

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

## SirWilliam — rejoining requires a full game relaunch

**Channel:** Discord · **Reported:** after leaving a session, rejoining does not
work until the game is fully restarted · **Status:** filed, not yet fixed

Filed as a session-lifecycle row from the same b125 triage map. Reproduction and
fix are queued; the row stays here, openly unfixed, rather than being quietly
dropped.

---

## Testers — "all peers can't move forward (W doesn't work)"

**Channel:** direct, 2026-08-29 mid-test · **Status:** OPEN, not reproduced here, needs a log

Reported during a test of the overnight b143 build: every peer was unable to move
forward. Three suspects were measured and cleared the same day, so the next attempt
starts from facts rather than from scratch:

- **The movement path itself is intact.** `tools/mp.py navprobe` on a rig where BOTH
  peers wore a converter scientist skin on their own body
  (`client_model: -> skin 'luther_v1sc' (mesh 2/2 slots, atlas tex bound)`) reported
  `GATE B(AddMovementInput)=PASS`, best displacement 78 060 cm. So swapping the local
  body to a pak skin does not break locomotion.
- **We do not take the W key.** The overlay's WndProc consumes exactly F1, `VK_OEM_3`
  (tilde), `T`, `V` and ESC-while-chat-open (`ui/imgui_overlay.cpp:208-290`). A
  tree-wide grep for `DisableInput` / `EnableInput` / `SetIgnoreMoveInput` returns
  nothing.
- **No spurious knock-out.** Zero `ko_respawn: death backstop fired` lines in either
  peer's log across the session's smokes.

What would settle it in one step, and what the next session should ask for: whether
**A/S/D and the mouse** still work (all keys dead points at our `CaptureActive()` — a
surface stuck open swallows every key; only W dead points at game state, not input),
**which build** was running (the overnight zip, r2modman, or a dev install), and the
host + client `multivoid.log` covering the moment. The remaining untested suspects are
a stuck `ko_respawn` KO pin (it re-ragdolls the player while `g_active`, so a KO that
never completes reads exactly as "cannot move") and the КПП join teleport firing
repeatedly if the local-pawn cache flickers.

---

## Maintenance note

This file and the two short tables move together: a fix that came from a report
adds a row **here** and a line in **both** short tables (README §Field reports,
`site/templates/index.html` §07). The site is deployed by hand — the built
`site/public/` must be regenerated and uploaded for a site-side change to appear.
