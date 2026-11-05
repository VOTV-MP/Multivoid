# Credits — reports and reviews

The long record of every outside **report** and **review** that changed the mod: what was reported or argued, what it
turned out to be, and what shipped because of it. The ledger rows and the code contributions are in
[credits.md](credits.md), which the README and the website summarise.

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
   answering yes for a world that was gone. Every cached engine
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
built and saying, in public and in good faith, that a decision in it was mistaken.
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
public repo, an un-annotated replaced decision is ammunition.

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
measured each, and closed the gaps the measuring found (the `coop/props/container_*`
lane). The prompt is Moddy's; the invariants, the measurements and the fixes are this project's
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
