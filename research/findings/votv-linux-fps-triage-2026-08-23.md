# Linux 9-fps triage — the absorbed-AV storm, the 1 Hz walk stack, and the join-flood recurrence (2026-08-23)

**Session directive (user, 2026-08-23): investigation ONLY.** Nothing was built, fixed, or
launched this session; this document is the sole artifact. Every fix below is a PRESCRIPTION,
not a change.

**Inputs** (`ignore_folder/linux_fps_issue/`):
- `discord chat.txt` — Violet (host) reports a friend "running the game at like 9 fps…
  She doesn't have these issues in base game". Both on Linux via **GE-Proton11-5-x86_64**.
- `multivoid_friend_with_fps_issue.log` — the friend (CLIENT), 12.35 MB over a 6-minute
  session, b133 (compiled Jul 31 2026), exe under `Z:\home\playerone\…` (Proton).
- `multivoid_violet.log` — the host, 1.35 MB, same b133, healthy.

**Verdict in one paragraph.** Three separate problems overlap in the friend's session, and
"9 fps" is their sum. (R-1) A **new root-cause finding**: after playing a solo save and
quitting to the menu, `players::Registry::Local()` keeps serving the dead world's pawn —
the world-transition invalidation that the code's own comments promise (`InvalidateLocal()`)
**has zero call sites** — and `input_owner`'s 1 Hz full scan then feeds that dead pawn's
dead PlayerController into `HasUserFocusedDescendants` on every live UserWidget: **~2,508
absorbed access violations per second for 44 seconds**, each SEH-caught and logged (78% of
the 12.35 MB log is this one ERROR line), consuming the game thread through the whole
join flow. Proton amplifies the per-AV cost; the mechanism itself is OS-independent.
(R-2) The **known** 2026-06-23 L5 root recurs at field scale: the per-channel GUObjectArray
walks cost the friend a 65–305 ms our-code hitch every 1–2 s (host: same shape, ~9× cheaper).
(R-3) The **sustained** ~11–13 fps floor during actual gameplay is measured NOT to be our
game-thread tick (whole `net_pump::Tick` < 10 ms on those frames) — attribution between the
engine on that machine vs our uninstrumented passives (PE-detour overhead, overlay draw) is
open, and the already-built `perf_probe` is the discriminator to run next. (R-4) The b125
§R-A reliable-loss shape recurred at 3× volume: **485 PropSpawn sends dropped at enqueue**
(`rc=-25`) in the join minute, plus a new finding — the client broadcast **~1,600 DESTROYs
of its own join-culled locals** because the world-load-episode suppression covers keyed
destroys but not eid-only trash clumps.

---

## §1 The friend's session, phase map [M]

| Window | Phase | Evidence |
|---|---|---|
| 23:15:46–23:16:06 | boot → health PASS → boot menu | header; `HEALTH: PASS`; `world=preLoad` |
| 23:16:26 | menu world loads (958 deferred dispatches drained; menu button injected) | `pump drain … 958` + injection lines |
| 23:17:17–23:17:28 | save-picker open (`gameOwnsText -> YES via kbfocus`) | edge lines |
| 23:17:31 | **SOLO gameplay world loads** (1,666 deferred; swapchain rebuilt; **no** menu-injection lines) | `pump drain … 1666` + `swapchain resized` |
| 23:17:32–23:18:45 | solo play, mod idle (no session → no pump → **no instrumentation**) | near-silence (5 lines) |
| 23:18:47 | **quit to menu** (2,631 deferred drained; menu re-injected) → **R-1 STORM BEGINS same second** | injection lines, then AV #1 at 23:18:47 |
| 23:18:57 | MULTIPLAYER clicked; browser refresh (2 servers) | |
| 23:19:07 | BeginConnect ('My VOTV Server') | |
| 23:19:31 | last storm AV (engine purge finally kills the solo world's objects mid-load) [H on the purge] | AV per-second counts drop to 0 |
| 23:19:38 | ClientWorldReady; inertPawn spawned; **BeginSnapshot (3,093 objects)** | |
| 23:20:06 | Snapshot **Complete — applied 2,607/3,093, 28 s**; 871 pile proxies spawned; the client cull-DESTROY burst (R-4) | |
| 23:20:06–23:21:40 | **gameplay at ~11–13 fps**: 8–13 `[HITCH]` frames/s at 54–94 ms; exactly ONE `[HITCH-SRC]` (~65 ms) per 1–2 s | §3, §4 |
| 23:21:40–23:21:50 | quit to menu (no second storm — reaper mass-purge sees the world dead by 23:21:45), session stop | |

The host log is healthy throughout: **zero** AV lines, hitches only around its own world
load and the join minute, WS ~3.1 GB (friend ~3.4 GB — unremarkable).

---

## §2 R-1 [measured] The absorbed-AV storm: stale cross-world PlayerController × per-widget engine call × absorb-and-continue

### Symptom numbers
- **33,490** `[ERROR] game_thread: PE detour-outer-callback AV caught --
  function='HasUserFocusedDescendants' …` lines = **9.64 MB of the 12.35 MB log (78%)**.
- Window 23:18:47 → 23:19:31 (~44 s). Peak sustained **~2,508/s** (per-second line counts are
  ~constant at 2,508–2,541 for 12 straight seconds — i.e. one full sweep of ~2,508 live
  UserWidgets takes ≈ 1 s of game thread and is re-scheduled at its 1 Hz cadence:
  the game thread spends the window almost entirely inside absorb + log).
- Every line: same fault ip `VotV-Win64-Shipping.exe+0x26A7489`, `access=FFFFFFFFFFFFFFFF`
  (the Windows convention for a **non-canonical pointer deref / #GP-class AV** — i.e. the
  engine dereferenced scribbled memory), `self=` differs per line (the sweep's widgets).
- `HasKeyboardFocus` (called first, takes no controller) never faults; only the
  controller-taking descendants call does → the poisoned input is the PC, not the widgets.

### Root chain (code-verified, four layers)

1. **The warm cache accepts a dead-world pawn.** `players_registry.cpp:100-133` warm path =
   `localCached_.Alive() && !IsPuppet(...)` — `Alive()` is GUObjectArray slot+serial only
   (deliberately no per-frame Controller check per the 2026-06-08 quadbike lesson). It has
   **no world identity** and no PendingKill consideration — and per the recorded lesson
   `[[lesson-dying-world-actors-not-killflagged-at-menu]]`, a torn-down world's actors at the
   menu are not even kill-flagged, so `Alive()` stays true until the engine actually purges.
   After solo-quit the cache served the dead pawn for **44+ s**.
2. **The invalidation edge was never wired.** `Registry::InvalidateLocal()`
   (`players_registry.cpp:169`) exists, and `input_owner.cpp:116` documents it as fired
   "on a level change or a respawn" — **grep finds ZERO call sites** (only the declaration,
   definition, and two comments). The world-transition edge that the mod detects in several
   places (the spawn-refusal window arm, `world_load_episode`, the reaper's world-name diff)
   never tells the registry.
3. **input_owner feeds the stale PC into an engine call per widget.** The 1 Hz full scan
   (`input_owner.cpp:332-335`) resolves `g_pcForScan = GetController(Local())` —
   `GetController` is a reflected UFunction call (`engine_pawn.cpp:82`) that *succeeds* on
   the dead pawn's intact memory and returns the dead PC — then the sweep
   (`input_owner.cpp:347-359`) calls `HasUserFocusedDescendants(pc)` on **every live
   non-`Default__` UserWidget**. Inside the engine, the dead PC's controller→player chain
   holds scribbled memory → constant-ip AV per widget per sweep. (~2,508 widgets answered
   each sweep; plausibly the dead solo world's widget population still in-array plus the
   menu's [H].)
4. **The absorb firewall has no storm behavior.** Each AV is caught by the inner PE
   dispatch's outer SEH (`pe_detour.cpp` `RunDetourSEH` → `LogObserverAv`, the
   "detour-outer" label), logged at ERROR with a `NameOf/ToString` allocation + format +
   file write, and absorbed. `R::CallFunction`'s caller **cannot see the failure** (the
   absorb happens below it; `ret=false` is indistinguishable from "no focus"), so the sweep
   marches on: 2,508 absorb+log cycles per sweep, no latch, no abort, no dedup.

### Why Proton made it a 9-fps complaint, and why Windows never saw it
- Under Wine/Proton each absorbed AV is a SIGSEGV → Wine exception-dispatch round trip plus
  our log line (8.4 MB written in ~45 s through the Wine fs layer) — expensive enough that
  one sweep ≈ 1 s. On Windows the same storm would run at a fraction of the per-AV cost
  (still: thousands of ERROR lines and a mangled log per quit-to-menu).
- **All four local install logs contain 0 instances** — because the autonomous/test flows
  always join from the boot menu and never do *solo save → quit to menu → join*, which is a
  completely ordinary player flow ("play a bit, then join your friend"). The mechanism is
  OS-independent; the flow was simply never exercised locally. Also measured: the
  *connected*-world quit at 23:21:42 did **not** re-storm — the reaper's mass-purge saw that
  world's objects dead within ~3 s, so the stale window there was too short. The storm's
  length is engine-purge-timing dependent; the solo quit left the pawn slot-live for 44+ s.

### Two more consumers of the same root, same window [M]
- **Puppet spawn retry read a scribbled component pointer per frame:** `remote_player.cpp:97`
  → `puppet.cpp:109-124 GetMeshPlayerVisibleAsset(local)` logged
  `puppet: local skin = (comp=00000000FFFFFFFF asset=0000000000000000)` +
  `AnimInstance NULL (comp=00000000FFFFFFFF)` — 94× each, per frame during the join window:
  `ReadPtr` on the dead pawn returned `0xFFFFFFFF` as the mesh component. (Guarded reads
  returned null; unlatched, it retried and logged every frame.)
- **`K2_GetActorRotation` absorbed ×93** (constant `self`, ip `+0x2FA83E6`) during 23:19 —
  some per-frame consumer holding one stale actor across the same transition. Not chased to
  its call site this session; same staleness class.

### Fix prescription (root-first, RULE 1)
- **(a) Wire the invalidation edge.** The world-transition detector the mod already owns
  (one authority — the spawn-refusal window arm / `world_load_episode` edge) calls
  `Registry::InvalidateLocal()`. Census the sibling cached engine-object refs while there
  (`engine_pawn.cpp CamMgr`, `input_owner g_lastOwner/g_localPawn`, etc. — the D1
  `CachedObjRef` sweep of 2026-08-22 is the inventory to walk).
- **(b) Make the warm path's validity predicate correct for a cross-world cache.** Stamp the
  resolved pawn's UWorld (outer chain) at resolve time; the warm path compares the stamp
  against the current world — pointer reads only, no PE call, no alloc, so it respects the
  quadbike constraint that banned the per-frame Controller check. (a) alone fixes this
  instance; (b) makes the NEXT unwired edge unable to recreate a 44-second stale window.
  They are one fix at two layers, not alternatives.
- **(c) input_owner keeps re-resolving through the registry** (it already does, per scan) —
  with (a)+(b) its `g_pcForScan` becomes null/fresh across transitions. No per-widget
  validation crutch is needed or wanted.
- **(d) Targeted sweep-abort hardening (principle 4-compliant).** Surface the absorbed-fault
  fact to the caller (`t_lastTaskFault` is already captured; expose a fault-generation
  counter read), and the full scan aborts its pass when its callee faulted. Any future
  same-class bug degrades to ≤ 1 absorbed AV per second instead of 2,508.
- **(e) OPEN QUESTION for the user (diagnostic policy, do not build unilaterally):** should
  `LogObserverAv` dedup/rate-latch same-`(function, ip)` absorbs (log first N + a count)?
  The storm made the log 78% one repeated line; a latch preserves diagnosis without the
  12 MB. This is log policy, not behavior suppression — but it changes crash forensics, so
  it is the user's call.
- The exact engine sub-function at `+0x26A7489` was not derived this session (no local
  `.i64` found on a bounded search; the mechanism is proven without it). If wanted:
  `tools/debug/ida_aob_derive.py` header documents the `idat.exe` invocation.

---

## §3 R-2 [measured; KNOWN root recurring] The 1 Hz walk stack = the once-per-second our-code stutter

- Friend, whole log: `sync:interactable` n=120, **median 74 ms, p90 104 ms, max 305 ms**;
  in the same seconds `sync:atv`/`keypad`/`power`/`turbine` ~13 ms each and `sync:grime`
  ~14 ms — a stacked our-code frame of ~65–130+ ms about once per 1–2 s (**exactly one
  `[HITCH-SRC]` per 1–2 s in the gameplay window, ~65 ms**, e.g. 23:20:46 paired with
  `sync:interactable = 64,691 us`).
- Host, same build, same session: `sync:interactable` n=51, median 7.5 ms, max 141 ms —
  same shape, ~9× cheaper machine. This cost exists for everyone; a weak machine turns it
  into a visible per-second stutter.
- The root is **already proven** (memory `project_L5_fps_hitch_root_2026-06-23`): the six
  interactable channels each full-walk GUObjectArray (~237k objects then) on their rebuild
  cadence; `device_occupancy` was co-dominant in that measurement. What ships today is the
  stream-settle discipline + 60 s staggered backstops (`interactable_channel.h:470-524`,
  `kRetryRebuildThrottle = 2 s`, `ue_wrap/settled_object_scan.h`) — which *bounded* the cost
  but left per-channel full walks that stack and that scale with machine speed and world
  churn (join-window churn re-arms full walks: the join minute shows interactable at
  70–75 ms every 2 s).
- **Fix direction** (resume the 2026-06-23 design fork recorded as "pending user"): ONE
  shared settled-scan pass per tick that feeds ALL class indexes (the extraction point
  already exists), plus a global stagger so at most one full walk lands on any frame.
  Precondition from the two failed migration takes: the **N-match gate** (door 57 count
  regression) must pass — take-1 (pure tail-scan) and take-2 (settle gate) both failed it;
  the shipped take-3 shape is the baseline to preserve.

---

## §4 R-3 [measured negative + open] The sustained ~11–13 fps floor is NOT our game-thread tick

- In the gameplay window (23:20:30–23:21:19) the friend renders **8–13 `[HITCH]` frames per
  second at 54–94 ms — essentially every frame** — and on those frames there is **no
  `[HITCH-SRC]`**, i.e. our entire `net_pump::Tick` (which contains ALL sync ticks, the
  reaper, the deleter, puppet drive) took **< 10 ms**. Our direct per-frame game-thread work
  is exonerated for the floor; §3's stack rides on top of it as the once-per-second spike.
- What the floor could be — **undecidable from this log**, and the recorded lesson
  `[[lesson-periodic-hitch-not-the-walk-by-period-coincidence]]` forbids attributing by
  plausibility: (i) the engine itself on that machine rendering the HOST's lived-in world
  (a joiner inherits the host save: 2,607 snapshot objects + 871 pile proxies + full prop
  population — a heavier world than her own fresh base-game save) under DXVK; (ii) our
  **uninstrumented passives**: the PE-detour per-dispatch overhead (Bloom + bounded observer
  walks × VOTV's dispatch rate) and the overlay's per-frame render — both run inside engine
  call stacks, invisible to `[HITCH-SRC]` *by construction*.
- **The discriminator already exists and was OFF in this run:** `[dev] perf_probe=1`
  (+ `perf_probe_selftime=1`) arms per-dispatch counting and 1-in-256 self-time sampling
  (`pe_detour.cpp SetPerfCounting`, rows `config_registry_rows.inc:259-261`) and reports
  per-second tables. **Prescription:** the next Linux report gets a run with both flags on;
  compare PE self-ns + the bucket table against the frame time. Note also the solo window
  was uninstrumented (the pump only ticks with a session), so "base game fine" vs "mod
  loaded but idle, solo" is not yet separated either — the same probe run answers both.
- Context: WS friend ~3.38 GB / host ~3.12 GB — unremarkable for a loaded VOTV world.

---

## §5 R-4 — **§5 AS WRITTEN IS PARTLY WRONG; read §5-CORR first (2026-08-23, same day)**

### §5-CORR — what re-grepping the same two logs actually shows

The original §5 below was written from a plausible reading, not from grepping each claim. Three of
its statements are false and one causal chain was inverted. Corrected by counting, in the same two
files, before any of it was built on:

| §5 claim | Measured |
|---|---|
| "At SnapshotComplete the **membership sweep** culls the client's unclaimed locals" | **FALSE.** The sweep **ABORTED**: `claim sweep ABORTED -- would destroy 957 of 1829 in-universe actor(s) (>50%)` at 23:20:13, and `dooming` appears **0 times**. It destroyed nothing. It also *cannot* be the source even when it runs: it retires through `prop_lifecycle::DestroyLocalProp`, which echo-suppresses (`prop_destroy_seam.cpp:226`). |
| "the suppression fired **for keyed props (65×)**" | **FALSE — 2,172×**, all at 23:19. The gate worked, and worked at scale. |
| "the host **parked 940** as `[DESTROY-DEFER]` and TTL-dropped them" | **1,618 parked, exactly 1 dropped.** The client broadcast **1,629** destroys total: **940 eid-only clumps at 23:20:01** + **689 KEYED at 23:20:06+** (`eid=0`). |
| implied: the burst happens *inside* the episode, so widening the gate covers it | **In the FIELD the episode had already CLOSED** — `load-tail QUIESCED ... episode CLOSED` at **23:19:38**, 23 s before the burst. **Locally the same burst lands INSIDE the episode** (reproduced 1:1: 871 broadcasts / 871 host defers). So the gate's key-scoping and the gate's end-condition are **two independent defects**, and each hides the other depending on machine speed. |

**The causal chain, re-derived.** The sweep's abort is not a separate finding — it is R-4b's
consequence: 485 `PropSpawn` sends were refused at enqueue (`rc=-25`, silent), so the host's snapshot
arrived genuinely incomplete (2,607 of 3,093 applied), so 957 of 1,829 client locals were unclaimed,
so the >50% ratio valve correctly refused to destroy half the world. **The valve did its job.** The
284 parked `container_contents` (282 expired) are the same root one hop further down. R-4b is
therefore not a "structural, secondary" item — **it is the thing that broke the join.**

**Why the episode closed early (field only).** The quiescence probe latched *population stable* at
23:19:38 — the same second `ClientWorldReady` fired and `BeginSnapshot(3,093)` began. It measured a
lull immediately BEFORE the host's snapshot started pouring in, and the world rebuild then ran to
23:20:13. `Arm()` is once-per-world-load and `ArmQuiesceProbe` does not re-raise `g_inEpisode`, so
once closed the episode never reopens. The end condition needs the snapshot bracket, not population
stability alone — two independent signals for one question.

**Status:** the key-scoping half is FIXED (the `!keyless` scoping removed; before/after measured
locally 871 → 0). The end-condition half is NOT fixed and needs its own `/qf`. The 689 post-episode
KEYED broadcasts at 23:20:06+ are **unattributed** — do not assume they share a root with the 940.

---

### §5 (original, superseded in the four rows above)

- **Recurrence:** the HOST logged **485× `net: SendReliableToSlot(slot=1) rc=-25 kind=3`**
  — kind 3 = **PropSpawn**, rc=-25 = GNS enqueue refusal (send buffer full) — all inside
  the join minute (23:19). This is exactly b125 triage **§R-A** ("reliable" loss at enqueue,
  silent, contiguous; ~60 call sites ignore the false return; fix direction already named:
  pace the drain against the send-buffer budget / ONE delivery-guarantee owner) — **still
  unfixed, recurring in the wild at 3× the b125 volume** (was 164).
- **NEW — the client broadcasts its own join-cull destroys.** At SnapshotComplete the
  membership sweep culls the client's unclaimed locals; each cull runs through the
  grab_hook destroy-seam, which **broadcast ~1,600 DESTROYs** (1,147 in 23:20:0x + 413 in
  23:20:1x; 940 of them eid-only trash clumps) to the host — which parked 940 as
  `[DESTROY-DEFER]` (destroy-before-load) and TTL-dropped them. The world-load-episode
  suppression exists and fired **for keyed props (65× "suppressed KEYED DESTROY … inside
  world-load episode")** but does NOT cover the eid-only clump path — an asymmetry: local
  bookkeeping culls become wire traffic + host work exactly during the flood window.
- **NEW — the downstream expiry chain closes end-to-end:** client
  `container_contents: eid=N not resolvable yet -- parked` ×284 → **282 expired** (contents
  permanently missing for eids whose spawn/bind never arrived — the 485 dropped PropSpawns
  are the obvious feeder [H on the exact eid overlap]); plus `[PROP-DROP] client
  pending-place cap 32 hit` ×242 — join churn arriving OUTSIDE the `world_load_episode`
  window flooded the place-detection queue, i.e. **the episode latch ends while churn
  continues** (same latch-window mismatch the DESTROY burst shows).
- **Fix direction:** (root) join-cull destroys are local bookkeeping — extend the episode
  suppression to the eid-only clump path and re-derive the episode latch's end condition
  against the snapshot bracket (two independent signals today); (structural) the §R-A
  delivery-guarantee design pass — queued-until-sent or connection-fatal, never
  warn-and-drop (`docs/LESSONS.md` entry + b125 §R-A carry the shape); (secondary) pace the
  snapshot drain against the send budget (b125's named fix).

---

## §6 Small residuals + context (so nobody re-derives them)

- **Snapshot apply took 28 s** (3,093 objects, applied 2,607) on the friend's machine —
  loading-screen phase; noted, not judged.
- **871 pile proxies spawn in one burst** at Complete — by design (rooted `AStaticMeshActor`
  NoCollision trash-pile proxies); context for the world-content delta a joiner inherits.
- `skin_registry: no starter-list pak present -- falls back 'hl_einstein_v1sc'` — the
  **known WP-9 scan-dir blocker** observed in the wild (decision already recorded 08-23:
  one shared `scientists.pak` ships in the mod package; silent-fallback surface census in
  `[[project-rtss-dx12-and-thunderstore-2026-08-23]]`). No new action.
- Fresh-ini `net.nick=Pelmentor` on the friend's machine is **deliberate**
  (`config_registry.h:38-41`, user-ruled default; the host arbiter renames dupes). Context
  only — do not "fix".
- `weather: applied flags …` prints ~every 2–3 s (×52) at receive cadence — log noise only.
- The storm turned a 6-minute session into a 12.35 MB log (78% one line) — see §2(e), a
  user-decision question.
- `multiplayer_menu: menu restored for connect` fires on every menu-world load regardless of
  connecting (naming confusion while reading logs; harmless).

## §7 Facts Pelmentor can relay to Violet's friend

1. The join-window freeze and a large part of the bad first impression is a mod bug we have
   root-caused (it triggers after "play solo → quit to menu → join"); the fix lands at the
   identity layer, not a workaround.
2. The sustained low-fps floor inside the joined world is measured NOT to be the mod's
   per-frame game-thread work; whether it is the machine rendering the host's heavier world
   or the mod's passive hook overhead is exactly one diagnostic run away: set
   `perf_probe=1` and `perf_probe_selftime=1` under `[dev]` in `multivoid.ini`, play a
   minute, send the log.
3. Locked-game joining (unlock → join → re-lock) was already answered in the chat; a proper
   invite/allow flow is a product gap, not part of this defect set.

## §8 Prescription summary

| Row | Layer | Fix | Size | Precondition |
|---|---|---|---|---|
| R-1a | players_registry ← world-transition edge | wire `InvalidateLocal()` from the ONE world-change authority; census sibling `CachedObjRef` holders | S | pick the authority (spawn-refusal arm vs world_load_episode) |
| R-1b | players_registry warm path | world-identity stamp on the cached pawn; compare on warm read (pointer reads only) | S | none |
| R-1d | input_owner sweep | abort pass when a callee absorbed a fault (expose fault-generation counter from `t_lastTaskFault`) | S | R-1a/b first (this is hardening, not the fix) |
| R-1e | absorb-path logging | same-(function,ip) dedup/rate-latch | S | **user decision** (crash-forensics policy) |
| R-2 | interactables + sibling pollers | one shared settled-scan feeding all class indexes + global stagger | M | resume the 2026-06-23 fork; N-match gate must pass |
| R-3 | measurement | `perf_probe` run on the affected machine (and one solo run) | — | reporter cooperation |
| R-4a | grab_hook destroy-seam / world_load_episode | suppress eid-only clump culls in-episode; re-derive the episode end condition vs the snapshot bracket | S–M | none |
| R-4b | session send boundary | the §R-A ONE delivery-guarantee owner design pass | M–L | /qf per standing rule before impl |

**Provenance:** all numbers grep-derived from the two logs in
`ignore_folder/linux_fps_issue/` on 2026-08-23; code cites verified against the working tree
at `50b78d47` (+ held WIP). Confidence tags: [M] = measured this session, [H] = hypothesis
explicitly marked inline.
