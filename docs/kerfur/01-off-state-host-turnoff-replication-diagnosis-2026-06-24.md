# 01 — OFF-state non-replication on HOST turn-off (diagnosis)

> **CORRECTION (2026-06-24, same day, user re-test): THIS SCENARIO (turn_off on a FULLY-CONNECTED client)
> is likely NOT the bug -- out of the join window the convert channel is CLEAN.** The real bug is a
> **JOIN-WINDOW timing race** (host ACTIVATES kerfurs during the connect window), with THREE distinct
> symptoms -- the same window two-channel class as the L1 pile dup, NOT a convert-channel failure. See
> **`02-window-activation-three-symptoms-2026-06-24.md`**. This doc's static analysis (the convert channel
> is fully built + correct OUT of window) stays valid + is WHY 02 concludes "window-timing, not the channel."

**Status: SUPERSEDED as the bug hypothesis by doc 02 (window race). The static channel analysis below is
still the ground truth for how the convert channel works.**
Opened 2026-06-24 (user hands-on observation, NEW track -- not a regression of current work).

## The bug (user's words)

- Each kerfur has TWO states with their OWN menus: active **kerfur NPC** and **kerfur object (off)**.
- Host **turns ON** -> active kerfur NPC appears -> **client ALSO sees it. This path WORKS.**
- Host **turns OFF** -> host gets a kerfur object (off prop) -> **client sees NOTHING (голяк).**

## The user's 4 questions — answered by code/RE (fact, not guess)

**Q1. Off object vs active NPC — same actor toggling, or different classes?**
DIFFERENT CLASSES, destroy+spawn (not a flag). Active = `kerfurOmega_C` (NPC). Off = `prop_kerfurOmega_C`
(prop). `turn_off` = `dropKerfurProp()` spawns the prop at the kerfur's transform + `K2_DestroyActor`s the
NPC. (RE `votv-kerfur-convert-RE-2026-06-12.md` :116-117; design header `kerfur_convert.h` :8-18.)

**Q2. Active kerfur sync channel?**
`npc_sync`. The host registers its kerfur NPC as a HOST-OWNED `Npc` Element (`m_mirror=false`) in the
shared `MirrorManager<Npc>` (npc_sync.cpp:427, :121) and broadcasts EntitySpawn/EntityPose. That is why the
active kerfur appears on the client (and was a clean control in the L1 pile test).

**Q3. Does the OFF object go through that channel? Why not replicate?**
NO -- it goes through a SEPARATE dedicated channel: **`kerfur_convert` -> `KerfurConvert` broadcast** (the
off form is a prop, not an NPC, so npc_sync does not carry it). **This channel IS fully built** (v67-v78):
the host's death-watch poll detects the NPC->prop conversion and `ConvergeAfterConversion` ->
`kerfur_entity::BindFormActor` **broadcasts `KerfurConvert`** (kerfur_entity.cpp:227,288, host-gated). So
the user's hypothesis ("the off-object spawn isn't on the working channel") is refined by fact: the off
object is NOT meant to ride npc_sync; it has its OWN channel -- and the bug is that channel FAILING for the
host-initiated case, NOT a missing channel.

**Q4. Menu binding?**
The menu is the actor's NATIVE menu (`actionName` for the NPC, `actionOptionIndex` for the prop) -- NOT
separately synced. So menu replication is DOWNSTREAM of actor replication: if the off prop materializes on
the client, its menu comes with it; if the prop never appears, there is no menu because there is no actor.
The menu is not a separate bug.

## The full host-turn-off path (as-built, what SHOULD happen)

1. Host radial menu `turn_off` -> `dropKerfurProp` [PE-invisible]: spawn `prop_kerfurOmega` at the kerfur
   transform + `K2_DestroyActor` the NPC.
2. Host `PollKerfurConversions` (5 Hz, kerfur_convert.cpp:559): the host-owned kerfur `Npc` Element's actor
   is now DEAD while the Element is still present (and was cached LIVE in `g_kerfurWatch`) -> fire
   `ConvergeAfterConversion(toProp=1)` (host branch, :621-624).
3. `ConvergeAfterConversion` (:186): not sentient -> `FindNewFormKerfurActor(wantNpc=false)` finds the new
   prop within 5 m of the kerfur's captured pose -> `RegisterHostPropSilent(newProp)` -> `ReleaseNpcElement
   Silent(oldEid)` -> `BindFormActor(... Form::Prop ...)` -> **broadcasts `KerfurConvert`**.
4. Client `OnKerfurConvert` (:821): destroy old NPC mirror (`npc_mirror::OnEntityDestroy`) +
   `MaterializeKerfurMirror(toNpc=false)` -> synthesize a `PropSpawn{key="coopkerfur#<eid>"}` ->
   `remote_prop_spawn::OnSpawn` materializes the off-prop mirror.

**Static verdict: every step exists, is host-gated correctly, and looks correct for the NORMAL (non-flesh-
room) case.** The Z=20000 flesh-room spawn (which WOULD defeat `FindNewFormKerfurActor`'s 5 m search) only
happens `when isInFleshRoom` -- NOT a base kerfur -> refuted as the normal-case cause.

## Where it can break (candidates -- a repro log decides which)

The path is heavily logged at every step, so ONE host+client log of the repro localizes the break exactly:

1. **Poll never fires** -> NO `kerfur_convert: POLL turn_off (kerfur NPC eid=N died invisibly)` on host.
   Cause would be: the host's kerfur NPC was not in `MirrorManager<Npc>` / not cached LIVE in
   `g_kerfurWatch` before the toggle (e.g. a freshly-purchased kerfur turned off within one 200 ms poll
   before it was cached live; or a typename that does not contain "kerfurOmega"). -> client gets nothing.
2. **Converge can't find the new prop** -> `kerfur_convert: turn_off converge -- no new kerfur prop near
   (x,y,z); releasing dead NPC eid=N (no broadcast)` (:203). NO broadcast -> client gets nothing. (Would
   need the prop to spawn outside 5 m -- flesh room, or a spawn-timing race where the poll catches the NPC
   death before the prop is queryable in GUObjectArray.)
3. **Broadcast sent but client drops it** -> host logs `BindFormActor ... KerfurConvert broadcast` but
   client has NO `applied KerfurConvert` -> a wire/routing/slot-0-gate gap in event_dispatch_state.
4. **Client materialize fails** -> client logs `applied KerfurConvert` but the prop never appears, or
   `cannot resolve class '...'` / the synthetic PropSpawn `OnSpawn` fresh-spawn fails.

## NEXT (decisive step)

Get a **host + client log of the repro** (host turns off ONE base kerfur, client watches). Grep, in order:
host `POLL turn_off` -> `no new kerfur prop near` (gate #2) / `BindFormActor ... KerfurConvert broadcast`
-> client `applied KerfurConvert` / `cannot resolve class`. The FIRST missing line in that chain is the
break. The `kerfurtoggle` autonomous harness (`coop/dev/kerfur_toggle.cpp`, env-driven) may reproduce the
HOST-turn-off path without a human -- check whether it covers host-initiated (vs client-initiated) before
relying on it; if not, this is a user hands-on (after L1's push-block lifts, per the queue).

**Discipline note:** the path being fully built means this is likely a subtle runtime gap (poll caching /
converge timing / a recent regression), NOT a missing feature. Do NOT propose a fix until a repro log names
the failing step (L5 lesson: fact not guess; Safety-1 lesson: do not assume the runtime behavior).
