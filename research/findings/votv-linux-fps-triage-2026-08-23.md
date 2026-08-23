# Linux 9-fps triage — the absorbed-AV storm, the 1 Hz walk stack, and the join-flood recurrence (2026-08-23)

**Original session directive (user, 2026-08-23 day): investigation ONLY.** Nothing was built that
session; every fix below was written as a PRESCRIPTION.

> **STATUS 2026-08-23 eve — PARTLY BUILT, AND TWO PRESCRIPTIONS WERE WRONG. Read §8's status table
> FIRST**, then §5-CORR and §4-CORR before acting on any prose below them.
> - **BUILT + drilled:** R-1 (the storm root, `88e29669`+`3f7b2b4e`), R-1e (the rate latch,
>   `86ff94a2`), R-4a's key-scoping half (`65fccd70`, measured 871 → 0).
> - **The prescription for R-1a could not work** and was dropped for a delete; **§5's whole R-4 story
>   was false** (the sweep aborted and doomed zero; 2,172 not 65; 1,618 not 940) and the causal chain
>   inverts — R-4b is what broke the join, not a secondary.
> - **R-3's prescription is dead:** the reporter cannot run probes.
> - Verification is autonomous `mp.py smoke` + purpose-built drills. **Nothing here is hands-on.**

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
  per-second tables.
- Context: WS friend ~3.38 GB / host ~3.12 GB — unremarkable for a loaded VOTV world.

### §4-CORR — the prescription is DEAD; R-3 is answerable from THIS end (2026-08-23 eve)

The original prescription was "the next Linux report gets a run with both flags on". **USER, verbatim:
*"I can't make Violet run things with probes unfortunately we're in this alone."*** R-3 was the only
row that ever depended on reporter cooperation, so that prescription is retired, not deferred.

It does NOT follow that R-3 is unanswerable. The replacement plan needs nobody:

1. **Measure our own passive overhead locally**, both flags on, on a join. The measured negative above
   already exonerates our per-frame game-thread work (whole `net_pump::Tick` < 10 ms on 54–94 ms
   frames), so what remains attributable to us is exactly two things, both invisible to `[HITCH-SRC]`
   by construction: the PE-detour per-dispatch cost and the overlay's render-thread draw. Those are
   precisely what `perf_probe_selftime` samples.
2. **Project it using the scaling factor her own logs supply.** The SAME walk cost 74 ms median on her
   machine vs 7.5 ms on the host (§3) — ~10×. So a locally-measured per-frame overhead projects onto
   her hardware with a measured multiplier instead of a guess. Worth the order-of-magnitude BEFORE the
   run, because it shows the question is not rhetorical: at ~150k dispatches/s a 200 ns detour overhead
   is 30 ms/s here and ~300 ms/s projected — either negligible or a third of her frame budget, and
   nothing currently known separates those. *(Arithmetic, not measurement — which is the argument FOR
   measuring, not a substitute for it.)*
3. **Run the A/B nobody has run:** the HOST's heavy save, SOLO, mod loaded vs mod absent, on this box.
   Her log could never separate "base game fine" from "mod loaded but idle, solo" because the solo
   window is uninstrumented (the pump only ticks with a session). Ours can.

**Honest ceiling, stated now so it is not discovered late.** If (1)+(2) return "our passives are
small", we will have EXPLAINED the floor — her hardware + DXVK + a world heavier than her own save —
without FIXING it. That is a real result and it is the honest close for R-3, but it is not her getting
playable frames. Two levers help her REGARDLESS of how attribution lands, and need nobody:
- **make the inherited world lighter** (a joiner takes the host's whole population — 2,607 snapshot
  objects + 871 pile proxies in one burst; proxy LOD / distance culling / deferring non-essential
  spawns at join are all ours to choose), and
- **make the discriminator cheap enough to ship ON**, so the next report of this class arrives
  pre-attributed instead of needing a round trip through a human who cannot make it. The always-on
  `[perf]` lines already carry the frame + PE rates in her log; it is the per-dispatch self-time that
  is gated behind an ini flag nobody will ever set. That is the ROOT fix for this whole class of
  "user reports slow, we cannot reach them".

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

## §5b NEW ROW (2026-08-23 day 3, found during the R-2 phase-split): reseed:KnownKeyedProps stalls the HOST

Re-mining VIOLET's (host) log with per-label walk totals: `reseed:KnownKeyedProps` n=17,
avg **120 ms, max 1,880 ms** — nearly 2-second single-frame stalls on the healthy machine, the
largest single [WALK-TIME] anywhere in either field log. Separate walker (prop reseed cadence),
NOT part of the SettledObjectScan family R-2 covers; also visible small on the friend
(n=7 avg 30 ms). Own row, own dig — do NOT fold into R-2's hub design. Also measured while
phase-splitting: `sync:npc_client` on the friend = 42 ms avg ×25 (mirror interp/apply, not an
array walk; the ghost-sweep walk is once-per-world) — a third row if it recurs after R-2 lands.

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
   per-frame game-thread work. ~~whether it is ... is exactly one diagnostic run away: set
   `perf_probe=1` ... send the log.~~ **RETRACTED 2026-08-23 eve — the reporter cannot run
   probes (USER), so nothing here is relayable.** Attribution is now ours to do from this end;
   see §4-CORR. Do not draft another "please set these flags" message.
3. Locked-game joining (unlock → join → re-lock) was already answered in the chat; a proper
   invite/allow flow is a product gap, not part of this defect set.

## §8 Prescription summary — STATUS AS OF 2026-08-23 eve (`86ff94a2`)

Every row re-checked against the code this pass. **Two prescriptions were WRONG and are not what
shipped** (R-1a, R-4a) — see the CORR sections; a prescription that survives contact unchanged is the
exception, not the rule.

| Row | Status | What actually happened |
|---|---|---|
| **R-1a** | **DROPPED — the prescription could not work** | "Wire `InvalidateLocal()`" is self-defeating: `RescanLocal`'s refill filter had the SAME blind spot (`IsLive` + non-null `Controller` both pass on the dead pawn), so an immediate re-walk re-caches it. `InvalidateLocal()` was **DELETED** instead (`88e29669`) — zero callers for its whole life, and one invariant gets one mechanism. |
| **R-1b** | **BUILT + drilled** (`88e29669`) | The world stamp, but on `CachedObjRef` (the shared cross-tick cache type, ~67 uses) rather than the registry alone — it was three bugs in one 44 s window, not one. `Set()` stamps, `Alive()` compares; teardown cannot defeat it because the object is never re-read. NEGATIVE CONTROL ×3: object/slot/serial held constant, world poisoned → `slotLive=1 aliveBefore=1 alivePoisoned=0`. |
| **R-1b'** | **BUILT** (`3f7b2b4e`) | `RemotePlayer::Spawn`'s `FindObjectByClass` fallback judged the same way — it could otherwise hand back exactly the pawn `Local()` had just learned to refuse. Found by a 72-site census of every `Local()` consumer (5 teardown-sensitive, 12 gates, 55 steady). |
| **R-1d** | **STILL OPEN** | `reflection.cpp:88-93` still `return true` unconditionally at `86ff94a2` — the absorb happens below it, so the caller cannot see a faulted call. Needs a thread-local fault-generation counter + a census of the 61 `CallFunction` sites. Now hardening rather than the fix, since R-1b closed the source. |
| **R-1e** | **BUILT + drilled** (`86ff94a2`) | USER-APPROVED. First 5 lines per distinct `(function, ip)` in full, then folds and re-reports on decade boundaries. Keyed on `(function, ip)` NOT `self` — the storm had a constant ip and a varying self. DRILL (`VOTVCOOP_AV_LATCH_DRILL=1`): 120 calls → 7 lines, 3 at a new site → 3 lines, both peers, in a PASSING smoke. |
| **R-2** | **BUILT 2026-08-23 day 3 (the shared scan hub) — acceptance GREEN, NOT hands-on** | `coop/element/object_scan_hub` (8-round /qf "that holds"): ONE sliced shared pass replaces the 13 per-consumer full walks; 10.0× fewer walk visits, per-frame walk cost capped ~1-2 ms (was 66 ms local / 305 ms field spikes), N-match parity 13/13 vs an independent probe (June's numbers reproduced: door 50/light 42/...), the pre-existing dead-world index window (R-1 class) closed family-wide via gen-stamped indexes. Design + AS-BUILT: `architecture-audits/votv-shared-scan-hub-R2-DESIGN-2026-08-23.md`. Residual periodic hitch = reseed:KnownKeyedProps (§5b, own row). |
| **R-3** | **prescription DEAD, plan REPLACED** | The reporter cannot run probes (USER). See §4-CORR: measure our passives locally, project via the ~10× factor her own logs give, plus the solo heavy-save A/B. Honest ceiling: this may EXPLAIN the floor without FIXING it. |
| **R-4a** | **HALF BUILT** (`65fccd70`) | Key-scoping half shipped and measured 871 → 0 broadcasts / 871 → 0 host defers, 1:1. End-condition half NOT built and needs its own `/qf`. The doc's attribution of the burst was **false** — see §5-CORR. |
| **R-4b** | **BUILT 2026-08-23 (4 commits from `a39a19cd`)** | 6-round `/qf` "that holds" → `coop/net/send_backlog`: the ONE delivery-guarantee owner (per-(slot,lane) FIFO, absorbs only -LimitExceeded, hConn-stamped, 64KB pose-reserve, fatal bounds close-not-drop) + save-pump Begin success-gating + 4MB SendBufferSize + bracket-anchored park aging + client inbox pause-not-drop. Drill RED 956 losses → GREEN 0; throttled run: one 5s episode "all delivered", 0 expired parks, sweep 0 unclaimed destroyed. Design of record: `research/findings/network/votv-reliable-delivery-guarantee-DESIGN-2026-08-23.md`. NOT hands-on. |

**Residual audit items** from the post-ship audit of `88e29669` (one CRITICAL of my own making, fixed
in `86ff94a2`): UMG widgets Outer to the GameInstance not a ULevel, so the world term is **silently
inert for the whole widget surface** — documented in `cached_obj_ref.h`, not fixed. Plus the boot-window
null stamp, the non-atomic resolution globals, and `FindClass(L"Level")`'s first-match. Tracked as one
item; do not read the discrimination list in `cached_obj_ref.h` as complete without reading its gap note.

**Provenance:** all numbers grep-derived from the two logs in
`ignore_folder/linux_fps_issue/` on 2026-08-23; original code cites verified against the working tree
at `50b78d47`, status column re-verified at `86ff94a2`. Confidence tags: [M] = measured, [H] =
hypothesis explicitly marked inline. Before/after figures in the status column are from `mp.py smoke`
runs on this box (autonomous — NOT hands-on), same save both sides of each comparison.
