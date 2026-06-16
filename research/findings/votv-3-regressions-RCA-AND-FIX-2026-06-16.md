# Three hands-on regressions — RCA + proper fixes (2026-06-16)

Hands-on (host `Game_0.9.0n` + client `Game_0.9.0n_copy`, one PC, logs 11:43–11:48,
DLL build 11:36 = commit d2c7d958). User report: **"all peers can't interact with
objects, and kerfurs are still duping when a client turns them off and grabs them."**

Three DISTINCT root causes, found via 3 parallel RCA agents reading the live logs +
code. All fixed in commit **f920f713** (build clean, deployed x4). User (on PC) to
hands-on verify per [[feedback-user-tests-claude-prepares-ground]].

---

## (1) "All peers can't interact" = the Q spawn menu has no CLOSE path

**Root cause.** `ue_wrap::spawn_menu::Open()` (commit ed12b446, story-mode Q menu)
sets `SetInputMode_GameAndUIEx` + `bShowMouseCursor=1` but there is **no `Close()`
anywhere** and **no `SetInputMode_GameOnly` call in the whole codebase**. The dev Q
watcher (`spawn_menu_unlock.cpp`) only fired on the Q **rising edge** → `Open()`. So
the FIRST Q press stranded the player in UI/cursor input with no way back to the
world use-trace.

**Log proof (host):** `12227 [11:47:30] spawn_menu_unlock: Q-key spawn menu ENABLED`
→ `12236 [11:47:31] spawn_menu::Open: … GameAndUI input` → ~20 more Q presses (each
re-applying GameAndUI, user mashing to escape) → NO `SetInputMode_GameOnly` ever →
session end. Host opened 0 doors + 0 grabs after 11:47:31; the client (never opened
the menu) interacted fine the whole time. "All peers" = the user drove both
instances and pressed Q on the one(s) that got stuck.

**Fix.** Add `ue_wrap::spawn_menu::Close()` (restore `SetInputMode_GameOnly` + hide
cursor + collapse the widget + run `closed()` if present — the inverse of `Open()`,
mirroring vanilla's `@11555` close). Add `Toggle()` deciding from GROUND TRUTH (the
widget's live `Visibility` byte) so it self-corrects. The Q watcher now posts
`Toggle()` → Q both opens AND dismisses. The watcher uses `GetAsyncKeyState` (global,
bypasses input mode), so the second Q is seen even while "stuck". Files:
`ue_wrap/spawn_menu.{h,cpp}`, `coop/dev/spawn_menu_unlock.cpp`.

**Verify:** press Q (cursor appears) → press Q again → `spawn_menu::Close: restored
GameOnly input + hid cursor + collapsed widget` + interaction works again.

---

## (2) Join-time kerfur NPC dupe = a SPURIOUS second world snapshot

**Root cause.** Only ONE real world load on the join (one `open untitled_1`). But the
`prop_element_tracker` reaper drains the join's one-time **menu→game prop-element
shadow leak** (256/scan, 3561→746 over 11:44:00–45) and `net_pump`'s mass-purge
detector misreads that bookkeeping drain as a **"world change"** → sets
`g_reAnnounceWorldReady` → a **second** `ClientWorldReady` (11:44:49) → the host
re-replays the FULL snapshot → `npc_adoption::OnClientWorldReady()` RESETS the
per-world adoption state that had *already* correctly bound the 3 kerfurs
(eid 5255/56/57 at 11:44:00, "NO duplicate spawn") → `PruneDeadClientMirrors` drops
them (the prior actors read dead via the async load tail) → re-armed adoption finds
"no local twin" → **fresh-spawns duplicate mirrors** (24034/24037). Same root behind
the email re-storm (66× `applied email from slot 1` at 11:44:00) + 883
`BeginDeferred returned null` churn.

Every prior kerfur-dupe fix tuned the fresh-spawn TIMING (quiescence gates,
8s→60s timeout); none stopped the spurious re-announce or made the 2nd pass
idempotent. `ArmAdoption` IS already idempotent on eid (npc_adoption.cpp:183) — the
dupe only happens because the spurious re-announce drives `OnClientWorldReady` →
prune → re-arm.

**Fix (net_pump.cpp).** Gate the client re-announce on an ACTUAL UWorld swap. New
`static void* g_announcedWorld` = the UWorld at the last `ClientWorldReady` send; the
re-seed only re-announces when `reapWorld != g_announcedWorld` (a real cave/level
travel re-opens `untitled_1` = a NEW UWorld → still re-announces + re-binds keyless
chipPiles). Airtight: re-replaying host state into a "new" world only matters when
that world's actors were really destroyed+recreated — which only a UWorld swap does.
The join's same-world shadow drain no longer re-announces → no 2nd snapshot → no
reset → no kerfur re-adopt dupe. New shared helper `maybeReAnnounce` (one gate, both
re-seed branches). Reset `g_announcedWorld` on disconnect.

**Verify:** on a clean join the client logs `ClientWorldReady announced` ONCE (no
`-- re-announce after world-change`), plus `net_pump: world-change re-seed on the
SAME world already announced … NOT re-announcing`; ZERO `npc-adopt: … no local twin
… fresh-spawning` for already-adopted kerfurs; host `slot 1 world-ready` once.

---

## (3) off+grab kerfur dupe (the user's EXACT repro) = grab of an un-adjudicated prop in the join window

**Root cause.** A save-loaded turned-off kerfur prop (`prop_kerfurOmega_C`, key
`hHwsv9`) grabbed by the client at **11:44:23 — in the ~25 s window BEFORE the
divergence sweep arms** (the snapshot bracket only opened at 11:44:49). The grab fired
`trash_collect_sync::EnsureHeldItemBroadcast`, whose only suppression guard
(`IsPendingSweepCandidate`) is gated on `g_sweepPending` — **false** in that pre-arm
window — so the grab slipped through, `SendPropSpawn` ran, and the **host fresh-spawned
a PERMANENT duplicate** (`OnSpawn: spawned … of 'prop_kerfurOmega_C'`, eid 43938,
ownerSlot=1) that the client-side absence-sweep can never reclaim. NOT a
conversion/poll bug — the conversion poll is correctly gated off until quiescence;
this is the GRAB path. (The Init-POST broadcast path is NOT exposed: it `return`s for
Aprop_C descendants on the client, prop_lifecycle.cpp:193-194.)

**Fix (trash_collect_sync.cpp + remote_prop_spawn.{h,cpp}).** Factor
`IsInDivergenceUniverseUnclaimed(actor)` out of `IsPendingSweepCandidate` (the
universe test minus the `g_sweepPending` precondition — RULE 2, one membership def).
In `EnsureHeldItemBroadcast`, after the existing guard, also suppress when
`!HasLoadTailQuiesced() && IsInDivergenceUniverseUnclaimed(heldActor)` — i.e. the
WHOLE pre-quiescence join window: an un-adjudicated save-loaded local is not ours to
announce. The client keeps holding it locally (no pop); the host expresses its OWN
copy in the bracket (our held copy is then key/fuzzy-matched + claimed → the held-pose
stream mirrors it), or the sweep destroys ours if the host converted it away.
Claim/adopt, never a host fresh-spawn. `HasLoadTailQuiesced` flips true the instant
the sweep fires → gates ONLY the join window, never steady-state gameplay.

**Verify:** grabbing a kerfur prop during the join logs `trash_collect: … grabbed
PRE-QUIESCENCE (join window not yet reconciled) -- NOT expressing`; and the host logs
NO `remote_prop::OnSpawn: spawned … of 'prop_kerfurOmega_C'` from a client grab.

---

## Notes / debt
- net_pump.cpp 880 LOC, remote_prop_spawn.cpp 1273 LOC — both over the 800 soft cap
  (under the 1500 hard cap). Pre-existing; my changes were +25 / +10. Extraction
  proposals already standing: `world_change_reseed.{h,cpp}` (the net_pump reaper
  block), `prop_divergence_sweep.{h,cpp}` (remote_prop_spawn). DEFERRED.
- The runtime conversion poll (`PollKerfurConversions`) is gated on
  `HasLoadTailQuiesced` (client) — correct, asleep during the join window. The
  off+grab dupe was the GRAB path, not the poll.
