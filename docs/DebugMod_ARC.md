# DebugMod arc — making Multivoid and DebugMod friends (LIVE doc)

> **Canonical LIVING doc for the DebugMod coexistence arc.** Opened 2026-08-26 on the user's
> directive; this arc is a **part of the UE4SS arc** (`docs/UE4SS_ARC.md`) — it only becomes
> answerable because Multivoid is becoming a UE4SS mod that shares a process with other UE4SS mods.
>
> Status tags, same vocabulary as the UE4SS arc: **DECIDED** (ratified) · **AS-BUILT** (shipped) ·
> **PENDING** (built, unproven) · **DESIGN** (specified, not built) · **`[V]`** measured ·
> **`[RD]`** RE-derived · **`[?]`** unverified. **Nothing in this doc is BUILT yet.**
> Keep it current when a row moves; do not let a status label rot (`/documentize` rule).

---

## 0. THE ASK (USER 2026-08-26, verbatim)

> *"новая тема - она становится часть арки UE4SS - это арка DebugMod … The goal is a native user
> with Multivoid and DebugMod installed can fire different debugmod functions such as signal event
> fire, all kind of events fire, and Multivoid should be able to handle that, meaning a host with
> DebugMod + Multivoid session + friends, host firing all that is synced to his friends under
> Multivoid capabilities.*
>
> *Also what happens if a client starts a story event via debugmod, thats needs to be handled (only
> host owns firing events, but that doesnt mean clients who find a story signal on the workstation
> can't trigger any story event via that way or walking into a trigger box)*
>
> *Need to install DebugMod + Multivoid for this arc, so we can properly handle different things and
> research, and decide on how we make the two mods be friends to one another."*

**Two goals, and they are NOT the same problem:**

- **G1 — the HOST fires, everyone sees it.** A host running DebugMod + Multivoid triggers an event
  (story / trigger / ticker), a signal, a spawn, a world mutation; their friends see it. This is an
  OBSERVE-AND-RELAY problem: can Multivoid see what DebugMod did, and does a lane exist to carry it?
- **G2 — a CLIENT fires, and the answer is not "no".** The user's nuance is the load-bearing half and
  it is easy to misread: *"only host owns firing events, but that doesnt mean clients … can't
  trigger any story event"*. Event AUTHORSHIP is the host's. Event **CAUSATION** is not — a client
  who finds a story signal on the workstation, or walks into a trigger box, legitimately causes an
  event, and refusing that would manufacture a LOSS defect. So G2 is an INTENT problem in the
  `COOP_SYNCER_MODEL.md` §2b act-as-host shape, not a suppression problem.

**The failure mode this arc exists to prevent** is the one named in
`votv-ue4ss-coexistence-FACTS-2026-07-26.md`: *"the dominant risk is SEMANTIC not mechanical — a
world-mutating mod on one peer is adopted+amplified / fought / silently drifts."* DebugMod is
precisely a world-mutating mod, with an unusually large surface.

---

## 1. What DebugMod IS — measured 2026-08-26

| fact | value | evidence |
|---|---|---|
| package | `acitulen-DebugMod` 5.0.3, author Acitulen | `[V]` `manifest.json` |
| game target | **VOTV 0.9.0n** — the same build Multivoid targets | `[V]` its README warning box |
| shape | `mod/dlls/main.dll` (814 592 B) + `mod/enabled.txt` + `pak/DebugMod.pak` (1 337 587 B) | `[V]` `unzip -l acitulen-DebugMod-5.0.3.zip` |
| kind | **C++ UE4SS mod** using the `CppUserModBase` vtable | `[V]` imports `??0CppUserModBase@RC@@QEAA@XZ` |
| dependencies | `UE4SS.dll`, MSVC runtime, `Thunderstore-unreal_shimloader-1.1.7` | `[V]` import table + manifest |
| **sources** | **NOT PUBLIC.** `github.com/Acitulen/DebugMod` holds one branch (`5.0.1`), 50 commits, and exactly two kinds of file: `README.md` and `Preview/*.png`. There is no code in it. | `[V]` full clone + `git ls-tree -r` |

**Consequence of the last row, and it shapes the whole arc:** every coexistence fact about DebugMod
must come from its **import table, its pak, and runtime observation**. We cannot read its code, we
cannot diff its versions, and a 5.0.4 could change behaviour with no visible signal. Any invariant we
build on must therefore be one WE can observe at OUR seam — never one we assume about its internals.

### 1a. Where its pak goes, and why it is NOT our LogicMods blocker

`[V]` r2modman routes it to `shimloader/pak/acitulen-DebugMod/DebugMod.pak`; the manual guide says
`GAME/Content/Paks/LogicMods`. `[V]` Multivoid's `skin_registry.cpp:121` walks
`Content/Paks/LogicMods/`**`multivoid`** — a SUBDIRECTORY — so DebugMod's pak is not in our scan path
and does not collide. The `LogicMods` blocker recorded in `docs/THUNDERSTORE.md` is about OUR pak
needing a subdirectory Thunderstore's routing cannot produce; it is our problem, not a DebugMod one.

### 1b. DebugMod is also a working reference for WP-9's package shape

`[V]` Its zip is exactly the `mod/` + `pak/` wrapper that `UE4SS_ARC.md` §7.2a calls the
authoritative routing rule — the shape §7.2 originally got wrong by measuring an extracted profile.
It is a live, installed, working example of the layout Multivoid's own Thunderstore package must
produce. Cross-link kept deliberately: the two arcs share this evidence.

---

## 2. THE CENTRAL MEASUREMENT — can we even SEE what DebugMod does?

This is the question the whole arc turns on, and it is answered.

`[V]` DebugMod's import table carries exactly four hook/dispatch-relevant symbols from `UE4SS.dll`:

| imported symbol | what it means for us |
|---|---|
| `?ProcessEvent@UObject@Unreal@RC@@` | **it CALLS `UObject::ProcessEvent` to drive the game** |
| `?RegisterHook@UObjectGlobals@Unreal@RC@@` | UE4SS per-UFunction hooks (ProcessInternal / ProcessLocalScriptFunction — NOT a PE detour) |
| `?RegisterBeginPlayPreCallback@Hook@Unreal@RC@@` | UE4SS BeginPlay hook |
| `?RegisterCallFunctionByNameWithArgumentsPreCallback@Hook@Unreal@RC@@` | UE4SS CallFunctionByName hook |

**And the one that is ABSENT is as load-bearing as the four that are present:**
`RegisterProcessEventPreCallback` **does not appear**.

**Two conclusions:**

1. **DebugMod does not arm UE4SS's PolyHook ProcessEvent detour, so it cannot reproduce the
   double-detour crash class** (`UE4SS_ARC.md` §3).
   **These are two DIFFERENT classes of evidence and must not be filed as one.** §4 reached
   "no stock mod arms it" by INDUCTION over a mod population it cannot enumerate — that is a PRIOR,
   and a strong one, but it can never become a fact. An absent import is a FACT about this binary,
   at this version. So this measurement does not re-prove the induction and must not be cited as
   confirming it: it **removes DebugMod from the set the induction still has to cover**, and leaves
   every other mod exactly where it was. (Framing owed to the parallel session, 2026-08-26.)
   Note the scope carefully — it is a fact about **5.0.3**, and §1 already records that we cannot
   see this mod's source or diff its versions.
2. **DebugMod drives the game THROUGH ProcessEvent, which is the seam Multivoid owns.** So its
   actions are, in principle, VISIBLE to our detour — the opposite of the `lib_C::addPoints` case,
   where `[V]` all 19 credit sites dispatch `EX_LocalVirtualFunction` and are structurally
   unhookable (`COOP_SYNCER_MODEL.md` §2b step 2).

**The honest ceiling on conclusion 2, stated so it is not overclaimed later:** an import proves the
CAPABILITY and makes ProcessEvent the likely dominant path. It does **not** prove that any PARTICULAR
feature uses it. A feature that writes a property directly (DebugMod imports the whole `FProperty`
surface — `CopyCompleteValue`, `CopySingleValue`, `GetByteOffset`, …) mutates state with **no
dispatch at all** and is invisible to every hook either mod owns. **Per-feature verification is
runtime work and is §5 of this doc.** Do not let this table's green column become a claim that
"DebugMod is observable"; it says "DebugMod's function-call path is observable".

---

## 3. The feature surface, sorted by what it does to a SHARED world

From DebugMod's README `[V]`. The sort is ours: what matters is not the menu a feature lives in but
whether it mutates state two peers must agree on.

**Class A — mutates shared world state (the arc's real work).**
Event panel: run **story events**, **trigger events**, **ticker events**; monitor meta paranoia.
Signal panel: spawn drives with signals from all levels. Servers: break / fix / protect servers,
spawn items for today's task. Main menu: set time, day/night cycle speed, global time speed,
ariral reputation, **set player points**, base power control, all lights on/off, clean base walls
and floors, remove trash, clean window, fix radio tower, fix radar towers, reboot transformers.
Object locator: teleport objects to player, destroy objects. Extended spawn menu (with deferred
spawn). Portable base console terminal.

**Class B — mutates the acting player only (may still need per-peer routing).**
Immortality, prevent death, non-consumable satiety / stamina, infinite flashlight charge, disable
max-HP loss, disable bleeding, disable ragdoll, infinite inventory, player time speed, teleport
across levels, teleport to objects/waypoints, spectator mode.

**Class C — local/cosmetic, almost certainly out of scope.**
ESP, viewmodes, fullbright, object/property/function inspectors, waypoints, zone scanning, UI paging
and per-tick search settings, keybinds.

**Two Class-A rows deserve naming now because they cross lanes we have already secured:**

- **"Set player points"** lands on the same balance the `order_sync` and coin-gun work made
  host-authored. `[V]` The credit root `lib_C::addPoints` is PE-invisible at all 19 GAME sites — but
  DebugMod is not the game, and whether ITS write is visible is an open per-feature question (§5).
- **"Force save game, even during events"** collides with `save_transfer`'s snapshot instant.

---

## 4. The two goals as design problems

### G1 — host fires, friends see it

The shape is **observe-and-relay**, and Multivoid already owns the seam (§2). Per lane, the question
is the one `docs/COOP_SYNC_MAP.md` already answers for the game's own triggers: does a lane exist,
and does the DebugMod-initiated call reach the same seam the gameplay path does?

**The failure to design against is DOUBLE APPLICATION.** If DebugMod fires event X on the host and
our observer relays "event X started", a client that ALSO has DebugMod must not re-run it locally.
That is the `ScopedWireApply` echo-guard shape the desk-audio lane already uses.

### G2 — a client fires, and refusing is the wrong answer

**Restating the user's rule so it cannot be misread later: authorship is the host's; causation is
not.** Three distinct paths must be told apart, and only one of them is a cheat surface:

| path | who caused it | correct answer |
|---|---|---|
| client walks into a trigger box | the GAME, on the client | must work — ordinary gameplay |
| client finds a story signal on the workstation | the GAME, on the client | must work — the signal chain, already synced |
| client opens DebugMod's event panel and presses "run story event" | the CLIENT'S MOD | an INTENT: the client asks, the ARBITER performs |

`COOP_SYNCER_MODEL.md` §2b's four-step test applies to the third row verbatim, and its step 2 is the
one that decides feasibility: *can the arbiter OBSERVE the trigger, or can the actor point at an
ARTIFACT the arbiter can resolve?* An event NAME is an artifact — this is the `order_sync` shape (the
client names a `list_store` row, the host prices it), not the blocked-credit shape.

**And per §2b's own requirement, this lane must name WHICH SIDE IS SUPPRESSED.** It has to be the
CLIENT-SIDE PRODUCER — cancel the local fire at the input seam and send the intent — never a receive
gate on the host. A receive gate is exactly how a cheat fix manufactures a LOSS defect: the client's
world runs the event, the host's does not, and they diverge.

**OPEN, and it is a product question, not a technical one:** should a client's DebugMod event-fire be
ALLOWED at all, or refused with a message? Both are implementable. The user's phrasing ("that needs
to be handled") asks for a DEFINED answer, not necessarily a permissive one. **Not decided.**

---

## 5. What must be MEASURED before anything is designed

Nothing in §4 can be built until these are answered, and every one is runtime work on a rig with both
mods installed. `[?]` all.

1. **Per-feature dispatch census.** For each Class-A feature: does the action reach our ProcessEvent
   detour, or does DebugMod write the property directly? §2's ceiling makes this the first question,
   and the answer is per-feature, not global.
2. **Does DebugMod's `RegisterHook` overlap ours?** It hooks per-UFunction via UE4SS's ProcessInternal
   path; we hook ProcessEvent. Which UFunctions does it hook, and do we intercept any of the same
   ones? Two interceptors on one function is a defined-order problem we have not faced.
3. **Boot coexistence, both orders.** Multivoid + DebugMod in one process; does either mod's
   `start_mod` disturb the other? (Expected clean per §2 conclusion 1 — but expected is not measured.)
4. **The pak.** `DebugMod.pak` is 1.3 MB of LogicMods content. What does it add, and does anything in
   it collide with our converter paks or with `asset_load`?
5. **`enabled.txt` semantics for a two-peer rig.** A mod folder is LOADED but not STARTED when
   disabled; we already use this (`ArmPE` → `.off`). Confirm it is a clean per-install switch so a
   host-has-DebugMod / client-does-not asymmetry can be TESTED — because that asymmetry is the normal
   real-world case, not an edge case.

---

## 6. State

- **`[V]` DebugMod IS installed** in the r2modman test profile
  (`C:\r2modman\...\profiles\Default\shimloader\mod\acitulen-DebugMod`), alongside `Multivoid`,
  `ArmPE`, CrashContext, PBMovement, Fusion, FusionFix, VoidFax and the BP-loader family.
- **`[V]` DebugMod is now INSTALLED ON THE COOP RIG, deliberately ASYMMETRICALLY** (2026-08-26):

  | install | DebugMod dll | DebugMod.pak | Multivoid | why |
  |---|---|---|---|---|
  | HOST | yes (`155A3933…`) | yes | yes | G1: the host who fires |
  | CLIENT_1 | yes (`155A3933…`) | yes | yes | G2: the client who fires |
  | CLIENT_2 | **no** | **no** | yes | G1: the friend who must SEE it, and the §5.5 asymmetry |
  | CLIENT_3 | **no** | **no** | yes | second clean peer |

  Layout used is the manual one from DebugMod's own README (the rig runs UE4SS directly, not the
  r2modman shimloader): `Binaries\Win64\Mods\Acitulen-DebugMod\{dlls\main.dll, enabled.txt}` and
  `Content\Paks\LogicMods\DebugMod.pak`. `[V]` `LogicMods` was EMPTY before this — so the rig now
  has its first LogicMods pak, which makes §5.4 answerable and is itself worth watching.
- The asymmetry is the point: a symmetric install cannot tell "Multivoid relayed it" from "both
  copies of DebugMod did the same thing independently". CLIENT_2 is the peer whose screen proves a
  relay actually happened.
- Local package copy: `ignore_folder/thunderstore_mod_examples/acitulen-DebugMod-5.0.3.zip`.
- Nothing built. No lane touched. No boot has been run with both mods present yet.

## 6a. THE FIRST DUAL-MOD BOOT — RAN 2026-08-26, AND IT DID NOT TEST COEXISTENCE

`mp.py smoke` PASSED (both peers stable, client connected slot 1, movement_ledger + intent_authority
selftests ALL PASS, `[V]` 0 `[ERROR]` / 0 `HotPathGuard` in either log). **That PASS is a pass for
Multivoid ALONE, because DebugMod never ran.**

`[V]` Both peers:
```
Failed to load dll <...\Mods\Acitulen-DebugMod\dlls\main.dll> ... error code: 0x7f
Was unable to install mod 'Acitulen-DebugMod' for unknown reasons. Mod is not installable.
```
`0x7f` = 127 = `ERROR_PROC_NOT_FOUND` — a required import was missing.

**ROOT: a UE4SS ABI mismatch, and it is confirmed DISCRIMINATIVELY, not by correlation.**

| | UE4SS.dll | md5 | DebugMod |
|---|---|---|---|
| coop rig (4 installs) | 16 263 680 B, Feb 14 2024 = the **pinned 3.0.1** | `4c177b9e…` | **fails `0x7f`** |
| `Desktop09n` (r2modman launches it) | 16 228 864 B = the **experimental** build | `8a78269b…` | **starts** |

`[V]` The SAME DebugMod binary is logged starting under the experimental build:
`Mod 'acitulen-DebugMod' has enabled.txt, starting mod.` So it is the UE4SS version, not the install
layout, the pak, or our presence.

**THIS IS D-3'S CENTRAL DESIGN CHOICE, CONFIRMED BY A LIVE COUNTER-EXAMPLE ON THE SAME MACHINE.**
`UE4SS_ARC.md` §0 rejected the `CppUserModBase` C++ vtable as "ABI-unstable across UE4SS builds"
and took the C-ABI (`start_mod`/`uninstall_mod`) instead. `[V]` DebugMod uses `CppUserModBase` and
imports UE4SS's C++ symbols — and it does not load on a different UE4SS build. `[V]` Multivoid
imports ZERO UE4SS symbols and is logged starting on BOTH builds. Same game, same day, same box.

**A SECOND, INDEPENDENT BLOCKER, found in the same logs** — it would have bitten even with the ABI
fixed, and it is worth knowing because it splits the mod in half:
`[V]` DebugMod's pak is mounted not by DebugMod but by **`BPModLoaderMod`** (a Lua mod):
`[Lua] DebugMod == table:` / `AssetPath == /Game/Mods/DebugMod/ModActor`.

**CORRECTED 2026-08-29 — the HOST row below used to read "never mounted — no
`BPModLoaderMod`", and that was wrong.** `[V]` The HOST's `Mods/mods.txt` carried
`BPModLoaderMod : 1`, its own `UE4SS.log` printed `[Lua] DebugMod == table:`, and removing
`Content/Paks/LogicMods/DebugMod.pak` from the HOST bought **~14 fps** (~75 -> ~89) — a pak that
was never mounted cannot cost frames. The earlier claim came from reading `Mods/*/enabled.txt`,
which is NOT UE4SS 3.0.1's enable list; `Mods/mods.txt` is. Census the loader's real list, and
confirm against the loader's own log.

**So the rig holds these states, none of which is "DebugMod working":**

| install | C++ half | BP half (pak) |
|---|---|---|
| HOST | fails `0x7f` | **MOUNTED** `[V]` — the same PARTIAL-LOAD state as CLIENT_1 |
| CLIENT_1 | fails `0x7f` | **MOUNTED** `[V]` — a real PARTIAL-LOAD state |
| CLIENT_2 / CLIENT_3 | absent by design | absent by design |

**RIG STATE AS LEFT 2026-08-29 (evening) -- IT IS NOT THE BASELINE, CHECK BEFORE MEASURING
ANYTHING ON IT.** Three things on the HOST differ from a clean rig, and the last one invalidates
any perf number taken without noticing it:

1. `Mods/mods.txt` has FIVE entries enabled, not six -- `CheatManagerEnablerMod` is 0. The full
   six-mod set is preserved as `mods.txt.bak`. `BPModLoaderMod` IS on, so the HOST's BP half of
   DebugMod DOES mount again (that was not true earlier in the day).
2. `[dev] perf_probe=1` is back on in `multivoid.ini`. It emits ~4 WARN lines/second, which is
   harmless for perf (measured ~0 cost) but MASKS the log-flush behaviour -- every WARN flushes,
   so a rig with the probe on cannot reproduce a lost-tail log.
3. **`ue4ss.dll` IS NOT OURS.** It is shimloader's build (Git SHA `e31aaaa6`, 2026-05-07), copied
   in from the r2modman install for a loader experiment. The rig's original zDEV loader
   (`d935b5b`, dated 2024-02-14) is preserved beside it as `ue4ss.dll.zdev-backup`.

**THE ~45 fps ATTRIBUTION THIS NOTE USED TO CARRY IS WITHDRAWN.** It said the mod set cost the
frames. Measured the same evening on one save, one pinned mod DLL, the same window, moving ONLY
`ue4ss.dll`: zDEV `d935b5b` -> **80 fps median**, shimloader's `e31aaaa6` -> **106**. So most or
all of the cost may be the LOADER, not the mods it loads. NOT SETTLED: the new loader failed to
start `CheatManagerEnablerMod` (5 of 6), so the loader and that one mod are confounded, and the
de-confounding arm (old loader, same five mods) has NOT RUN. See
`memory/project-fps-regression-hunt-2026-08-29.md` and
`memory/lesson-a-bisect-proves-its-own-rig-not-the-component.md`.

CLIENT_1's split state is itself a finding: a DebugMod whose Blueprint content is live in the world
while its native half is absent is a configuration a real user can reach, and nothing warns anybody.

**THE FORK THIS OPENS, and it is the arc's real first question — bigger than "can we see its calls":**
`[V]` Multivoid runs on 3.0.1 AND on experimental; DebugMod runs only on experimental. So **a real
user who wants both must be on the experimental build**, and our pin to 3.0.1 (a deliberate choice
recorded in `UE4SS_ARC` §0) is what stands between them. Three exits — **none chosen**:
(a) move the rig (and the pin) to the experimental build — matches where DebugMod's users already
are, but changes our substrate and re-opens whatever the pin was protecting;
(b) do this arc's research inside the `a09n` / r2modman install, which already runs both — but it is
a single-instance setup and two-peer scenarios are the entire point;
(c) obtain a 3.0.1-compatible DebugMod, which we cannot build (no sources) and can only request.

## 7. Next

1. **Boot once with both mods, before anything else** (§5.3). Nothing below is trustworthy until a
   process has actually held both. Watch for: our `pe_diag` verdict still reading
   `POLYHOOK-COMPOSED`/`WE-FIRST`, DebugMod's own load line, and any LogicMods pak complaint.
2. Run §5.1's dispatch census — it gates every design decision in §4.
3. Only then design G1's relay and G2's intent lane, each through its own `/qf` pass.
