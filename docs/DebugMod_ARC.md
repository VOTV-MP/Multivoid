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
   double-detour crash class** (`UE4SS_ARC.md` §3). That section already concluded this for the
   realistic stack, but by INDUCTION ("no stock mod calls it"); this is the same conclusion measured
   directly from DebugMod's own import table. `[V]`, not `[RD]`.
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

## 7. Next

1. **Boot once with both mods, before anything else** (§5.3). Nothing below is trustworthy until a
   process has actually held both. Watch for: our `pe_diag` verdict still reading
   `POLYHOOK-COMPOSED`/`WE-FIRST`, DebugMod's own load line, and any LogicMods pak complaint.
2. Run §5.1's dispatch census — it gates every design decision in §4.
3. Only then design G1's relay and G2's intent lane, each through its own `/qf` pass.
