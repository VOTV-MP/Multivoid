# Snapshot adoption root causes — white cubes / 1300-destroy / zero piles (2026-06-10)

Status: **all four root causes fixed in one deploy train (protocol v54+v55), built clean;
audit + smoke evidence appended below.** This is the post-mortem + fix record for the
2026-06-10 hands-on that came back RED on the P1+P2 build (whose autonomous smoke had
passed — see "why the smoke missed it").

User-visible symptoms (hands-on, host = s_1234 save, client = fresh New Game):
1. White ~1m cubes around the office, eating client FPS; the cubes CARRIED the host's
   office-wall identity (client grabs cube → host sees his wall move; client grabs his
   own standing wall → host sees nothing).
2. "Office walls spawned in air and never matched host's world position."
3. Client had ZERO trash piles; a pile only appeared when the host grabbed + released one.
4. The client's world was mass-destroyed at connect (1300 actors, including all 392
   level-placed trashBitsPiles).

## Evidence chain (logs of the actual run, 12:43–12:48)

- HOST: `PLAY READY` 12:43:49 → reaper mass-purge episode 12:43:53–12:44:18+ (2400+
  dying boot-world prop elements drained at 256/4s; "world-change re-seed deferred to
  drain-complete"). CLIENT connected 12:44:20 — mid-transition.
- CLIENT bracket #1: `claim tracking ARMED` 12:44:22 + `claim sweep -- 1300 live,
  0 claimed, 1300 destroyed` SAME SECOND (the connect-edge snapshot enumerated the
  mid-purge registry → near-empty bracket; the P2 sweep treated it as the host's
  complete world).
- HOST: "net_pump: re-snapshot ready slot 1 after world-change re-seed" 12:44:29 →
  bracket #2 with 2307 candidates → client sweep #2: 330 live / 330 claimed / 0.
- CLIENT fresh-spawn histogram: 311 trashBitsPile_C (the host's placed piles,
  needlessly re-created as mirrors after sweep #1 destroyed the client's exact-key
  twins), **20 prop_C** (the white cubes), 19 mushrooms, 5 swinger, 1 chipPile…
- CLIENT fuzzy-bind histogram: 117 binds (53 prop_C, 19 mushroom, 17 swinger, 15
  asologoPiece, 8 torch…) — **the fuzzy branch is LOAD-BEARING; the planned RULE-2
  deletion is falsified.**

## Root causes (all bytecode/code-verified; agents ac912b54/ac942efe + workflow wf_443da5c9-514)

### RC1 — connect-edge bracket built from a mid-transition registry
`net_pump.cpp` connect edge called `prop_snapshot::TriggerForSlot` unconditionally; the
registry was known-incoherent (purge episode active, re-seed deferred) and the designed
re-snapshot heals it 7s later — a fine contract while snapshots were ADDITIVE, fatal
once P2 made every SnapshotComplete destructive. The empty bracket #1 destroyed 1300.

### RC2 — prop visual identity never crossed the wire (white cubes)
An `Aprop_C`'s look is NOT class state: `init()` (UCS, runs in FinishSpawningActor)
resolves mesh/mass/collision from the `list_props` DataTable row named by FName
`Name@0x0258`; the CDO default row is literally **'cube'**. The "office walls" are
broken-cubicle gib panels — `list_props` rows `cubicleP_0/1/2` on BASE class `prop_C`
(7 panels per broken `prop_cubicle_C`; the host's save stores them as
`Fstruct_save{class=prop_C, names[0][0]=cubicleP_x}` and SP's
`mainGamemode::loadObjects → loadData` restores scale + key +
Static/removeWOrespawn/frozen/sleep + lifespan + nametag + **Name**, then re-runs
`init()`). Our PropSpawnPayload carried class+key+transform with **scale hardcoded
(1,1,1)** and no Name/flags → mirrors constructed as the literal cube row, carrying the
host wall's eid (the "identity theft"), while the client's own un-bound cubicles stayed
as local doubles.

### RC3 — keyless piles structurally unstreamable while class-swept (zero piles)
`actorChipPile_C` is KEYLESS (plain AActor; VOTV saves piles via the keyless
"Load Primitives" path). The snapshot enumerate skipped key==None → the host's piles
were NEVER streamed; but the P2 sweep was class-based (no key check) → it destroyed
every client pile at every connect → client always ended at zero. v52's eid-based
grab/convert events were the only pile creation path — exactly what the user observed.
**Bonus falsification:** the real pile spawner is `garbagePileSpawner_C` (one-shot at
level load, SEEDED RandomStream, map-baked arrays → placement is map-deterministic
cross-machine; the prior "undergroundGarbageSpawner unseeded-RNG piles" claim in
votv-clump-pile-dupe-DECISIVE-RE-2026-06-08.md §1(A) is WRONG on both counts —
undergroundGarbageSpawner mints only `dirthole_item_C` loot mounds). Cross-peer pile
divergence is host STATE DRIFT (save round-trip, collection, converts), not RNG.

### RC4 — sweep universe ⊂ snapshot universe (orphan doubles survive)
The host's save no longer contains the intact cubicles it broke (and contains props at
moved positions with runtime crypto-random keys the client's fresh world lacks —
`lib_C::assignKey → generateRandomKey` Base64Url FNames; level-PLACED actors carry
author-baked keys that DO match). The 4-class sweep could not remove the client's
orphaned intact cubicles / unbound standing walls → permanent doubles ("never matched
host's position").

### Why the P1+P2 smoke missed all of this
`smoke_phystele --host-settle 30` waits out the transition (hides RC1); an autonomous
smoke can't see visuals (RC2); nobody asserted pile counts (RC3); fresh-vs-save worlds
diverge most in the office the smoke never looks at (RC4).

## The fixes (one deploy train, protocol v54+v55)

### F2 / v54 — prop identity parity (RC2)
`PropSpawnPayload` 168→200B: `+propName` (32B WireKey-shaped, the list_props row);
senders stream the REAL `GetActorScale3D` (new `ue_wrap::engine::GetActorScale3D`);
physFlags += `kStatic 0x08 / kSleep 0x10 / kRemoveWOrespawn 0x20` (dead `kAtRest`
constant deleted). Receiver Path C raw-writes Name@0x0258 + the 4 bools
(`ue_wrap::prop::WriteSpParityIdentity`, Aprop_C-gated) BEFORE FinishSpawningActor →
init()'s single UCS pass constructs the true prop (better than SP's own two-pass: no
cube flash). Fuzzy matcher `FindNearbySameClass` gained an `expectedPropName` equality
gate (same CLASS is not same PROP for generic prop_C — a 'cube' and a 'cubicleP_1'
must not fuzzy-merge). Senders updated: prop_snapshot DrainChunk, prop_lifecycle Init
POST + takeObj POST (flag reads now Aprop_C-gated — pre-v54 read stray bytes on
non-Aprop_C), trash_collect EnsureHeldItemBroadcast. RE: agent ac912b54285a9e263.

### Fork A / v55 — bracket coherence gate (RC1)
The adversarial breaker killed the episode-flag shape (detection-latency race + a
latent mid-episode ReconcileIndexThrottled regression) in favor of an **O(1)
seed-world stamp**: every `SeedWalk_` stamps the live gameplay UWorld it expressed
(IsLive + 'ntitled' name filter — FindObjectsByClass has no liveness filter and
mid-transition the dying old world sits at a lower GUObjectArray index);
`TriggerForSlot` refuses to open a bracket unless `HasSeededOnce() &&
IsRegistrySeededForCurrentWorld()` (IsLiveByIndex on the captured pointer — a world
swap's GC purge kills the stamp with ZERO latency; VOTV's boot/save-load re-issues
`open untitled_1` = a real UWorld swap even when the map name is unchanged). Deferred
slots are flushed by seed-GENERATION bumps at the top of DrainChunk (unconditional on
coherence, independent of the re-seed's added>0); a mid-drain stamp death aborts the
bracket WITHOUT SnapshotComplete (no sweep fires; the re-bracket's SnapshotBegin
clears+re-arms claims); all dequeues route through the gated TriggerForSlot
(DequeuePending_ also fixes the pre-existing one-dead-slot-stalls-the-queue bug); a
small-travel else-if re-seed in net_pump closes the sub-64-purge deadlock (the reap cap
256 > 64 means the same scan already drained the dead — no recycled-address window);
join_progress::BeginSnapshot accepts Connecting||Receiving so a post-abort re-bracket
refreshes the stale progress denominator. SeedKnownKeyedProps' function-local latch
promoted to the file-scope g_seededOnce (RULE 2: one latch).

### Fork B / v55 — adoption-universe symmetry + keyless pile expression (RC3+RC4)
Contract: **SWEEP(client) == EXPRESS(host) ∪ CLIENT-FORBIDDEN.**
- HALF 1: SeedWalk_ phase 2 mints Elements for keyless chipPiles
  (`MarkPropElement(pile, "", cls)` — the v52 eid lane); DrainChunk expresses them
  eid-only (key.len=0 → the receiver's existing eidOnly OnSpawn lane), stamps
  `chipType` unconditionally (was NEVER stamped in snapshots — every snapshot pile
  would have rendered variant 0), and `WatchPile`s every expressed pile (host-grab
  coverage: the pile grab morph is ProcessEvent-invisible; v52 only enrolled receiver
  mirrors + owner converts — NOTHING covered the host grabbing a snapshot-streamed
  pile). kMaxWatchedPiles 256→1024 + shed WARN.
- HALF 2: sweep universe = keyed `IsClassKeyedInteractable` + keyless chipPile lineage;
  keyless NON-pile actors (held clumps mid-flight, pre-Init Aprop_C, event clumps) are
  NOT expressible → never swept (also fixes a latent bug: the old 4-class sweep could
  kill a flying clump mirror at a re-bracket); mushroom7 (wire-suppressed) IS swept —
  parity with the client's connected-state destroy-on-sight. Claims widened:
  `RecordClaimIfTracking` public + called at the 4 SELF-announce sites (a client
  announcing a prop mid-bracket must not have its own prop swept — a gap NEITHER
  design agent caught, found by the breaker); local-held guard
  (`engine::IsMainPlayerGrabbing`) in the exact-key + fuzzy bind paths (claim +
  mirror-bind, NO reconcile/converge — a re-bracket must not break a live
  PhysicsHandle hold); the host→client PropSpawn eid gate relaxed to EITHER range (a
  bracket RE-EXPRESSES existing entities incl. client-born mirrors — the PropDestroy
  either-range precedent; a client sender stays peer-range-only);
  `RegisterPropMirror` head guard (same eid+actor → quiet no-op, kills per-re-bracket
  duplicate-Install warnings); sweep destroys run the OnDestroy-parity teardown
  (`remote_prop::ClearAnyDriveFor` — extracted from OnDestroy, one impl —
  + ReleaseMainPlayerGrabIfHolding + echo-suppressed DestroyLocalProp).
  RULE-2 deleted: `IsRngDivergentClass`, `ResolveRngDivergentBases`,
  `g_propFoodMushroomCls` (the "RNG-divergent class" notion is gone — the universe is
  the snapshot's own predicate).

### Fork C / v55 — client ambient-RNG suppression (steady-state divergence)
New `coop/ambient_spawner_suppress` (one feature per file): PRE-cancel interceptors on
`mushroomMaster_C::spawn` + `mushroomSpawner_C::spawn` (looping engine timers; children
self-reap via the engine SetLifeSpan(1800) independent of the cancelled BP event) +
`pineconeSpawner_C::ReceiveTick` (random-interval tick; output self-expires via
SetLifeSpan(600)). Gate = `session->running() && role()==Client` per dispatch
(running_ covers handshake+snapshot drain; the bare role() gate is a post-session
SP-bleed defect class — Stop never resets cfg_.role). kMaxInterceptors 16→24 (census
was 15/16 client-side; the detour cost is Bloom-gated + count-bounded, the constant
only sizes a cold array). **Deliberately NOT suppressed:** `garbagePileSpawner_C`
(one-shot at load, seeded, pre-connect — the sweep's domain) and
`undergroundGarbageSpawner_C` — whose EXISTING Inc-3 suppression row was DELETED as
bytecode-falsified (it mints only dirthole_item_C, outside the snapshot/broadcast
universe; suppressing it deleted the client's loot mounds with no host replacement).
Dirtholes are per-peer LOCAL by doctrine (COOP_SCOPE amended). MTA precedent:
CMultiplayerSA.cpp:828 population/car-generator disables (MTA disables unconditionally;
we runtime-gate per principle 6).

## Falsifications recorded this session
1. "Delete the fuzzy branch after P2 green" — DEAD: fuzzy binds the runtime-keyed
   unmoved population (117 binds in the run); v54 hardened it with the propName gate.
2. votv-clump-pile-dupe-DECISIVE-RE-2026-06-08.md §1(A) — corrected in-file (spawner
   misattribution; pile placement is deterministic).
3. garbage_sync Inc-3 "undergroundGarbageSpawner output rides the broadcast" — false;
   row deleted.
4. "Post-connect RUNTIME divergent-spawn verification (unverified problem)" — VERIFIED
   REAL and fixed at the result level (fork C + the adoption sweep).
5. The interceptor-table perf comment ("walk is 16 acquire-loads") was stale — the
   D4-2 Bloom+count-bounded walk is the real shape; comments refreshed.

## Deferred / follow-ups (tracked, not this train)
- remote_prop_spawn.cpp at 799 LOC == soft cap: next touch extracts the claim/sweep
  block to `coop/prop_adoption_sweep.{h,cpp}`. net_pump.cpp 1117 / event_feed.cpp 1289 /
  remote_prop.cpp 860 pre-existing overages (extraction proposals: the reaper/purge
  block; the event_feed per-kind handlers; prop_mirror_manager).
- Host SetLifeSpan-expiry mirror leak (native Destroy bypasses K2 observer → no wire
  PropDestroy; reaper evicts silently): pre-existing, now the dominant stale-mirror
  source for ambient classes; needs its own design pass (gate around purge episodes).
- Save-load re-bracket pile eid rebasing: host piles reborn from a save get NEW eids;
  the client's old mirrors are swept while new eidOnly spawns land co-located
  (one-frame doubles mid-gameplay, then converge) — accepted adoption mechanics.
- Door keysHash divergence host-vs-client (57 doors, hashes differ — a subset of
  triggerBase keys is runtime-generated): interactable_sync routes by key → those
  doors may not mirror. NOT user-reported yet; investigate separately.
- The other connect-edge replays (weather/time/sky/npc RegisterExistingWorldNpcs/
  teleport) still run immediately for a deferred joiner against a mid-purge host
  world — fold under the coherence predicate only if hands-on shows artifacts.
- SEPARATE pre-existing host crash: 2113 IsHeavy use-after-free faults
  (votv-coop.dll+0x135A08).

## ★ GATE HARDENING (post-smoke falsification, same day)

**Smoke #1 (--host-settle 0, the bug repro) FAILED and FALSIFIED the stamp-only
fork-A gate**: the host bracketed 88 candidates mid-purge (registry: 1249 elements,
1161 dying / 88 live) because the stamped UWorld was STILL ALIVE — VOTV's boot/
save-load flow can mass-purge registry elements without killing the world object
the seed stamped (the breaker's "mass dying has exactly two sources" analysis was
wrong for this engine flow). The widened fork-B sweep then amplified the damage:
client swept **3316 in-universe live / 88 claimed / 3067 destroyed**, the episode-end
re-seed re-streamed 3175 (proving HALF 1: "870 keyless chipPile element(s)" minted +
expressed), and the client ballooned to 9GB re-spawning the world it had just
destroyed (each swept-key spawn pays the O(N) FindByKeyString fallback scan — the
[[project-bug-prop-resnapshot-leak]] allocation pattern; "stale-index self-heal"
fired 0 times because the index ENTRIES existed, only their actors were dead).
FPS parity stayed 0.99 and zero crashes — the failure was purely world-state.

**The hardened gate (shipped, build-clean): three trigger signals + an enumerate
backstop, any one defers:**
1. `!HasSeededOnce()` — boot seed never ran.
2. Stamp dead (`IsRegistrySeededForCurrentWorld`) — a REAL world swap; O(1), zero
   latency. KEPT for the swap case.
3. `InPurgeEpisode()` — the reaper's mass-purge episode, NOW TRACKER-OWNED
   (`prop_element_tracker::SetInPurgeEpisode/InPurgeEpisode`; net_pump's
   function-local `sInPurgeEpisode` static replaced wholesale, RULE 2 — net_pump
   still owns every detection edge). This is the signal that catches the
   stamp-alive/registry-dead boot flow (the host logged "mass-purge detected" 15s
   before it sent the bad bracket — the information was always there).
4. Enumerate-time backstop in StartEnumerationFor: `dying > live` → defer without
   sending SnapshotBegin. Closes the ≤4s window before the reaper's next scan
   raises the episode flag. Not a tunable heuristic: steady-state dying ≈ 0
   (normal destruction K2-evicts inline) and stream-out bursts are small vs the
   ~2-3k live world — majority-dead can only mean a transition.
The mid-drain abort also checks the episode flag now.

**Also fixed from smoke #1:** `mushroomMaster_C`/`mushroomSpawner_C` suppression
never installed — the live UFunction name is `Spawn` (capital, per the CXX header
dump) while the bp_reflection asset dump lowercases it and our
`reflection::FindFunction` compares case-SENSITIVELY. Registered names corrected.
(Audit-grade lesson: a BP function name from bp_reflection dumps must be
cross-checked against CXXHeaderDump before use in FindFunction.)

**Correctness audit (agent, 0 CRITICAL / 2 WARN):** WARN-1 — TriggerForSlot consumed
the deferred flag before the IsSlotReady early-return, silently dropping a deferred
not-ready slot (the connect-edge retry was the only rescue) — FIXED (the early
return now re-defers). WARN-2 — remote_prop_spawn.cpp at the 800 soft cap —
extraction proposal: claim/sweep block → `coop/prop_adoption_sweep.{h,cpp}` on next
touch. NOTEs: pre-existing >800 files (event_feed 1289 / net_pump 1117 /
remote_prop 860) re-flagged; localPlayer null-safety verified
(ReleaseMainPlayerGrabIfHolding null-guards internally).

**Perf/hot-path audit (agent, full function-by-function table produced): 0 CRITICAL
FAIL.** Every HOT-reachable function is O(1) steady-state with a verified latch
(ambient Install = latch->throttle->resolve; deferred flush = 3 bools idle;
pile watch at cap 1024 = ~10-80 µs/tick worst; interceptor bump leaves the
Bloom-reject path byte-identical; the v54 stamps are per-broadcast not per-tick).
WARNs folded same-session: W-1 stale FreeId O(n) comment (deque push_front is O(1));
W-2 the seed-walk world-stamp tail now does an inline pointer-compare walk instead
of FindObjectsByClass (which allocated a ClassNameOf wstring per GUObjectArray
entry, doubling every seed walk); W-3 WatchPileAt overload reuses the drain's
already-read location (one fewer ProcessEvent per expressed pile). W-4 accepted
(sweep destroy burst lands under the join cover; chunk at ~32/tick only if a
mid-gameplay re-bracket ever hitches). W-5 pre-existing resolve-retry shape, noted.
File flags re-confirmed: remote_prop_spawn.cpp 819 (claim/sweep extraction =
the next commit on that file); event_feed 1303 approaching the 1500 hard cap
(extract the prop-family reliable cases before the next ReliableKind).

**Re-smoke #2 (gate repro, hardened gate): GATE PROVEN.** Host log 15:29:39
"DEFERRING slot 1 until the next re-seed" (the episode-flag signal), NO empty
bracket, ONE coherent bracket at 15:29:59 (3175 candidates, 0 dying) after
"re-seed added 3089 (870 keyless chipPile element(s))"; client: ONE ARMED, 2178
exact-key binds + 704 fresh spawns (the host's piles streaming for the first
time) + 90 fuzzy; ambient 3/3 installed ("mushroomMaster_C::Spawn" etc.) with
pinecone cancels firing client-side; FPS ratio 0.97; zero Fatal/faults. The run's
RSS FAIL (8.7GB at kill) was a MEASUREMENT artifact: the 75s monitor killed the
run mid-bracket, before SnapshotComplete -- at the exact moment the client
legitimately holds BOTH worlds (its own ~3316 + incoming mirrors) and the sweep
hasn't reclaimed anything. Re-run with --duration 150 below.

## ★ SECOND + THIRD smoke iterations (same day) — two more root causes peeled

**Iteration B (runs 2-3): the +61s GC-purge fatal = the sweep destroyed the CLIENT'S
OWN `prop_inventoryContainer_player_C`.** The host snapshot expressed ITS player
inventory container (keyed Aprop_C, in-universe); symmetrically the client's own
container (per-save key, structurally unclaimable) was "unclaimed" -> swept -> the
client fataled at the NEXT GC purge (~61s later, both runs, engine refs into the
player's own inventory). FIX: new `prop_lifecycle::IsPerPlayerPropClass`
(prop_inventoryContainer_player_C) -- a DISTINCT concept from
IsWireSuppressedPropClass (whose client call site DESTROYS local instances; a
per-player class is the opposite: never snapshot-expressed, never live-broadcast,
never swept, local instance LIVES). Wired at: snapshot enumerate-skip, Init POST
broadcast-skip, sweep universe-skip. Plus: the sweep now logs a DOOMED-CLASS
HISTOGRAM (a wrongly-universed class shows up named in the log, not as a delayed
crash) and pairs its destruction burst with `engine::ForceGarbageCollection()`
(KismetSystemLibrary::CollectGarbage, end-of-frame purge).

**Iteration C (run 4 + the user's first-frame screenshots): walls FELL from the air
on the client = the SP-parity stamp trusts the `sleep@0x02DD` SAVE flag, which is
not live physics state.** The host's settled cubicle panels carried sleep=false
(flag) with rigid bodies long asleep -> the stamp shipped **533 of 614 prop_C as
kSimulatePhysics** -> every mirror spawned simulating, fell from the spawn
transform, re-settled divergently (the falling walls the user filmed) and re-created
the P1 woken-physics drag (client 68 vs host 116 FPS). FIX: the snapshot stamps
kSimulatePhysics only when `!(Static||frozen||sleepFlag)` AND the root rigid body is
POSITIVELY awake (`engine::IsActorRootBodyAtRest` -- the sound HOST-side reader
from the reverted kAtRest experiment; only its client-side sleep call had been the
crash). A genuinely moving prop is held/thrown and the PropPose stream owns it, so
an at-rest mis-stamp self-corrects on the next pose packet.

**RUN-5 (the deployed build) -- the full gate-repro join, cold connect during the
host's world transition:** host DEFERRING -> ONE coherent bracket (3175 candidates,
0 dying) -> client: **3 / 614 prop_C simulating (was 533)** -> sweep `4257
in-universe live / 3050 claimed / 1018 destroyed` with a clean doomed histogram
(870 chipPile [the client's own piles, replaced by the host's 870 mirrors], 81
trashBitsPile [piles the host's save genuinely lacks -- collected], 50 prop_C
[drifted/orphan gibs], 10 rock, 5 swinger, 1 beer, 1 drive; NO containers, NO
cubicles) -> **client steady FPS 106 vs host 115 = ratio 0.92**, zero Fatal/AV,
both logs healthy to kill time. The harness's "a peer DIED" verdict was a PID-
tracking artifact (it latched onto a 0MB zombie from an earlier killed run; both
real processes logged healthily to the end) -- harness bug, noted below.

## OPEN ITEMS after this session
1. **Client join RSS footprint ~10.7GB transient/plateau** (host: 4.1GB, same
   world). The during-bracket climb (+6.6GB over ~14s) is NOT pending-kill garbage
   (the post-sweep CollectGarbage and the +61s periodic purge both leave the
   plateau intact in the surviving runs) -- it is live/allocator-held memory of the
   join burst (914 fresh actor constructions + 2178 binds + dispatch transients).
   Does NOT crash (runs 4-5 survived; the only deaths were the swept-container
   fatal, now fixed, and the harness's 12GB job cap being grazed pre-fix). Needs a
   dedicated profiling pass (UE stat memory / allocator high-water) -- next session.
2. **mp.py harness**: PID tracking latches onto zombie VotV processes from
   previous killed runs ("peers=1 PID...=0MB" + false "a peer DIED" verdicts) and
   the kill pass leaves "1 VotV still alive". Fix the process tracker (track by
   PID handle, not name re-query) + kill-tree.
3. **Deferred-join UX**: a client joining DURING the host's world transition plays
   in its own world for the ~40s defer window, then the adoption visibly replaces
   the world (swept locals vanish, host mirrors appear). Honest but jarring --
   consider keeping the join cover up until SnapshotComplete for deferred joins.
4. Hands-on re-test pending (the deployed build): walls should REST at host
   positions (no falling, no white cubes), client has the host's 870 piles,
   wall/pile grabs sync both directions, FPS parity.
5. The extraction backlog (remote_prop_spawn 819 -> prop_adoption_sweep;
   event_feed prop-family cases; net_pump world-watch block) + perf_probe=0 +
   perf_probe_selftime=0 in inis before ship.

## ★ SESSION END STATE (compact-prep, 2026-06-10 ~16:30) ★

**Deployed build (all 4 game folders, via the run-5 smoke deploy) = the final build**:
protocol v55, containing v54 identity parity + the HARDENED coherence gate + keyless
pile expression + full-universe claim sweep + IsPerPlayerPropClass exemption +
body-awake sim stamp + post-sweep ForceGarbageCollection + ambient suppression
(correct 'Spawn' casing) + all audit paper-cuts (W-1/2/3, WARN-1). Build clean.
Everything UNCOMMITTED (the working tree also carries sessions 2-5's uncommitted
work; commit only when the user asks).

**User-observation dispositions (their live-watch reports during the smokes):**
- "walls spawn in AIR and fall, host's lie placed" (+ first-frame screenshots) =
  the flag-vs-body sim stamp (Iteration C above). FIXED in the deployed build
  (3/614 simulating); hands-on must confirm walls now REST at host positions.
- "client picks up his own local wall -- host doesn't see" = on the OLD builds the
  client's local panels survived (no full-universe sweep yet) and were host-unknown
  by construction. On the deployed build the local panels are SWEPT at adoption and
  the host's panels arrive as KEYED mirrors (rekeyed/registered -> the held-pose
  stream resolves them), so post-join wall grabs sync. A grab DURING the ~40s defer
  window (host still loading) is still local-only until adoption replaces it --
  expected; see OPEN ITEM 3 (join-cover UX).

**Session-6 file inventory (this session's diff, on top of sessions 2-5):**
- `include/coop/net/protocol.h` -- PropSpawnPayload 168->200 (+propName), physFlags
  kStatic/kSleep/kRemoveWOrespawn (kAtRest constant deleted), v54+v55 history,
  kProtocolVersion=55.
- `include/ue_wrap/sdk_profile.h` -- Aprop_Name@0x0258, Aprop_removeWOrespawn@0x02D9,
  GetActorScale3DFn, CollectGarbageFn, PropInventoryContainerPlayerClass.
- `ue_wrap/engine.{h,cpp}` -- GetActorScale3D, ForceGarbageCollection.
- `ue_wrap/engine_mainplayer.cpp` (+engine.h decl) -- IsMainPlayerGrabbing.
- `ue_wrap/prop.{h,cpp}` -- GetPropNameString / ReadRemoveWOrespawn /
  WriteSpParityIdentity; FindNearbySameClass +expectedPropName gate; RULE-2 deleted
  IsRngDivergentClass + ResolveRngDivergentBases + g_propFoodMushroomCls.
- `ue_wrap/game_thread.{h,cpp}` -- kMaxInterceptors 16->24 + stale perf comments fixed.
- `coop/prop_element_tracker.{h,cpp}` -- world-coherence stamp (inline ptr-compare
  walk) + g_seededOnce promotion + SeedGeneration + Set/InPurgeEpisode + keyless-
  chipPile Element minting in SeedWalk_ (+keylessPiles count in seed logs).
- `coop/prop_snapshot.cpp` -- THE GATE (3 signals + majority-dead enumerate
  backstop), deferred slots + generation flush + mid-drain abort-without-Complete,
  DequeuePending_/ClearDrainState_ unification, keyless pile expression (eid-only +
  chipType + WatchPileAt), per-player skip, v54 sender stamp, body-awake
  kSimulatePhysics stamp.
- `coop/net_pump.cpp` -- episode flag rewired to the tracker (function-local static
  deleted), retriggerReadySlots factored, small-travel re-seed else-if, ambient
  Install call + include.
- `coop/join_progress.cpp` -- BeginSnapshot accepts Connecting||Receiving.
- `coop/event_feed.cpp` -- PropSpawn host-sender either-range gate; OnSpawn/
  DestroyUnclaimedDivergentProps take localPlayer.
- `coop/remote_prop.{h,cpp}` -- ClearAnyDriveFor extracted (OnDestroy uses it),
  RegisterPropMirror same-eid+actor idempotency guard, OnConvert passes localPlayer.
- `coop/remote_prop_spawn.{h,cpp}` -- OnSpawn(+localPlayer) + local-held guards
  (exact + fuzzy), RecordClaimIfTracking public, sweep rewritten to the expressible
  universe + per-player skip + doomed-class histogram + OnDestroy-parity teardown +
  post-sweep ForceGarbageCollection, design block rewritten.
- `coop/prop_lifecycle.{h,cpp}` -- IsPerPlayerPropClass (new), v54 stamps at Init
  POST + takeObj POST (now Aprop_C-gated flag reads), self-claims at both sites,
  IsWireSuppressedPropClass note updated.
- `coop/trash_collect_sync.{h,cpp}` -- v54 stamp at EnsureHeldItemBroadcast,
  self-claims (held item + convert pile), kMaxWatchedPiles 256->1024 + shed WARN,
  WatchPileAt overload.
- `coop/garbage_sync.{h,cpp}` -- undergroundGarbageSpawner suppression row DELETED
  (bytecode-falsified) + comments updated. `coop/npc_sync.cpp` -- comment.
  `coop/element/registry.cpp` -- FreeId O(1) comment fix.
- NEW `coop/ambient_spawner_suppress.{h,cpp}` (+CMakeLists) -- 3 PRE-cancel
  interceptors (mushroomMaster_C::Spawn, mushroomSpawner_C::Spawn -- CAPITAL S per
  the CXX dump; bp_reflection lowercases! -- pineconeSpawner_C::ReceiveTick),
  running()+Client gate, latch->throttle->resolve.
- `docs/COOP_SCOPE.md` -- ambient-spawner + dirthole-local amendment.
- Corrections: `votv-clump-pile-dupe-DECISIVE-RE-2026-06-08.md` (banner),
  `votv-client-world-divergence-architecture-2026-06-09.md` (fuzzy-delete
  falsified + pointer here).

**Post-compact pickup order:** MEMORY.md top entry -> memory/
project_snapshot_adoption_2026-06-10.md -> THIS doc (root causes -> gate hardening
-> smoke iterations -> OPEN ITEMS). The deployed build is current; nothing needs
rebuilding for the user's hands-on. NEXT = hands-on checklist: walls REST at host
positions + grab-sync both ways; client shows the host's ~870 piles (correct
variants) + pile grab/convert still works; no white cubes; no doubles near base;
FPS parity; client RAM during join (expect a heavy transient peak ~10.7GB -- OPEN
ITEM 1). After hands-on green: RSS profiling pass, harness PID-tracker fix,
extractions (remote_prop_spawn 819 -> coop/prop_adoption_sweep; event_feed
prop-family cases; net_pump world-watch block), perf_probe=0 + selftime=0 in inis.

---

# EVENING SESSION (2026-06-10 ~17:00) -- object-overlay dev tool + the 12GB-cap crash forensics

## 1. NEW DEV FEATURE: world object overlay (user request "overlay + layers checkboxes")

3D-projected debug labels on nearby world objects so a misbehaving actor is identified
by SIGHT, not guesswork. Files: NEW `coop/dev/object_overlay.{h,cpp}` (game-thread
half, nameplate->hud snapshot pattern); draw in `ui/hud.cpp DrawObjectOverlay`
(foreground drawlist, under nameplates); checkbox panel in `ui/dev_menu.cpp`
(Player > HUD); pumps call `object_overlay::Update()` next to `nameplate::Update()`
in harness.cpp; `InitFromIni` at boot.

- Layers (F1 > Player > HUD): **Object names** (class + Aprop list_props Name row --
  the 'cube' vs 'cubicleP_0' discriminator), **Net identity** (eid hex + MIRROR/LOCAL
  + key suffix; UNTRACKED in orange = local-only actor that CANNOT mirror-sync),
  **Physics state** (sim/rest + static/frozen/sleep). Master + per-layer checkboxes +
  radius slider (5-100 m, default 25). Status line top-right: shown / in-range
  tracked / untracked / registry totals.
- Color code: cyan = wire mirror, green = tracked local, ORANGE = untracked local-only.
- `[dev] object_overlay=1` (inserted in HOST/CLIENT/copy2 inis, LEFT ON) force-enables
  at boot; menu toggles anytime.
- Data: element Registry SnapshotActorsByType(Prop+Npc) + a GUObjectArray walk for
  UNTRACKED prop-family actors (IsKeyedInteractable universe -- deliberately NOT
  doors/appliances, those are channel-synced and would be false alarms). Refresh every
  ~2 s; projection ~30 Hz over <=96 entries; disabled cost = 1 atomic load/tick.
- Audit (feature-dev:code-reviewer): 0 CRITICAL, 2 WARN. WARN-4 FIXED (registry
  HostCount/LocalCount mutex 60x/s -> cached at refresh). WARN-1 REJECTED with
  in-code comment (IsLive-first reorder: IsLive itself derefs the object per
  reflection.h:58, the cheap-lineage-first order IS the SeedWalk_ idiom, and the
  reorder would run SEH-guarded IsLive on ~250k slots instead of ~3k matches).
- Visual proof: `research/handson_logs_2026-06-10/overlay_client_live.png` (labels +
  status line live on the smoke client).

## 2. THE 16:40 "client balloons and crashes" -- ROOT CAUSE: mp.py's 12GB commit cap

The crash the user watched was MY smoke run (mp.py launches apply a **12.0 GB
per-process Job commit cap** -- the 19GB-incident guard). Monitor curve (smoke log
`build/objoverlay-smoke.log`): client steady ~4.0GB -> bracket opens (~t=47s,
16:40:58) -> 5.6GB(t=43) -> 8.1GB(t=49) -> DEAD before t=54 (commit crossed 12GB;
instant Job kill = no Fatal line, 0.2MB corpse PID 8720). Host healthy (4.1GB) to
the end. Logs preserved: `research/handson_logs_2026-06-10/{host,client}_votv-coop_1641.log`.

- Client died at mirror **2979 of 3175**, 13 s into ingest, BEFORE SnapshotComplete ->
  **the adoption sweep NEVER RAN** -> everything downstream the user reported follows.
- Host side v55 machinery PROVEN again on this flow: DEFERRING slot 1 at 16:40:38
  (mid-transition) -> one coherent bracket 16:40:58 (3176 candidates, 0 dying) ->
  drain complete in ~1 s (100/tick).

## 3. A/B smokes (audit-fixed build, deployed all 4 hash-match)

- A: overlay OFF -> **PASS**. Client 4.0 -> 5.5 -> 8.2 -> 10.6 -> plateau **10.72GB**,
  both peers stable. (= run-5 signature.)
- B: overlay ON -> **PASS**. 5.86 -> 8.58 -> 10.66 -> plateau **10.74GB**, frames
  115/s, 0 ERROR, object_overlay exactly 1 log line (the init latch), labels render
  (screenshot above).
- VERDICT: overlay steady-state cost ~20MB RSS / no fps hit; the 16:40 death = the
  join-ingest commit ALWAYS comes within ~1GB of the 12GB cap and that run tipped
  over (margin/variance), NOT an overlay leak. The structural problem is the JOIN
  INGEST BALLOON itself (OPEN ITEM 1, now top priority): +6.7GB transient in ~13 s
  while processing 3175 OnSpawns (723 fresh deferred-spawns + 2256 converges + piles),
  zero GC in the window.

## 4. cubicleP -- the full mechanism (user: "spawn in air on client, fall; don't sync at all")

- The broken-cubicle gib panels (prop_C, Name rows cubicleP_0/1/2) are spawned BY THE
  GAME on a save's FIRST RUN, mid-air, and physics-settle. Host's s_1234 save: settled
  long ago, at rest. Client always boots a FRESH save -> ITS OWN panels spawn in air
  and fall -- that is SP behavior, visible during the join window.
- Host expressed 7 keyed cubicleP panels. Client log: 5 -> NO exact key (fresh save =
  different runtime keys), NO fuzzy (client panels fell >30cm away) -> spawned as NEW
  kinematic mirrors at host resting positions (physFlags=0x00 -- v55 body-awake stamp
  working); 2 -> fuzzy-bound within 30cm (v54 propName gate matched) + rekeyed.
- So pre-sweep the client base = host mirror panels + the client's own UNTRACKED
  fallen panels = DOUBLES; grabbing an untracked local does nothing cross-peer =
  "don't sync at all". The sweep at SnapshotComplete destroys those locals -- it
  NEVER RAN because the cap killed the client first. ONE blocker explains all three
  user reports. The overlay now shows this state directly (orange UNTRACKED vs cyan
  eid mirrors).

## 5. Open quirk (logged, not gating)

perf_probe arming is flaky: smoke-A client had perf_probe=1 yet never logged "probe
ARMED" (so no [perf] baseline); crashed-run client + smoke-B client armed fine.
Suspect a first-call race on the `static const` latch in `perf_probe::Enabled()`.
Dev-only; probe is slated to be turned OFF before ship anyway.

PE rates for the record: client in-world ~430-450k/s PE at 110-115fps BEFORE any
mirrors existed (16:40:57 sample pre-bracket) -- the pre-existing DLL-wide hot path
the probe was built to hunt; NOT this feature (overlay adds ~10k/s by construction:
~10k dispatches per 2s refresh + 160x30Hz projection).

## 6. NEXT (priority order)

1. **Join-ingest balloon reduction** (the real fix for the user's crash): instrument
   + pace the client's OnSpawn ingest -- mid-ingest `engine::ForceGarbageCollection()`
   every ~500 processed mirrors (KismetSystemLibrary deferred-GC, safe by
   construction) + RSS log before/after each GC to MEASURE whether UE reclaims
   (per-spawn BP-init transients) or the binned allocator holds pages. Decide
   further pacing from that data, not guesswork.
2. Hold the join cover until SnapshotComplete (OPEN 3) -- hides the falling-panels
   window AND the world swap.
3. perf_probe arming race; perf_probe=0 + selftime=0 before any real-play handoff.

---

# SAVE-TRANSFER ARC (2026-06-10 ~17:15) -- USER GREENLIT the architecture

USER (verbatim): "I dont want client to have the office walls spawn in air for fucks
sake, can we just pull all objects data at connecting time and place/spawn objects
naturally" -> then: "Yes, let's transfer hosts save but make sure client can't
steal that save".

DECISION: the joining client gets the HOST'S SAVE at connect and loads the world
FROM IT (the game's own loadObjects places everything naturally) instead of booting
a fresh divergent world and reconciling it after the fact. Design workflow
(connect-world-bootstrap-design, 9 agents) ran the same evening -- verdict in this doc
below / next session memory.

## ZERO-CODE DECISIVE EXPERIMENT -- PASSED (save portability proven end-to-end)

1. VOTV saves live in `%LOCALAPPDATA%\VotV\Saved\SaveGames` (SHARED by all 4 game
   copies on one machine -- the test setup). s_1234.sav = 17,226,903 B. data.sav
   (48KB, global) is separate from world slots.
2. Copied s_1234.sav -> s_cooptest.sav on disk; booted the CLIENT game copy SOLO
   with env VOTVCOOP_SCENARIO=play VOTVCOOP_SAVE=s_cooptest (the existing
   BootStorySaveBlocking/LoadStorySave path -- NO new code).
3. RESULT: "LoadStorySave -- loaded save 's_cooptest'" clean; GameMode prefix-fix
   ran; seed walk 2434 keyed + 34 keyless piles from the host's data; screenshot
   `research/handson_logs_2026-06-10/savetransfer_test_1.png` shows the office-wall
   cubicleP panels LYING SETTLED ON THE GROUND at host positions (no mid-air
   first-run spawn -- the save is not first-run). The object overlay labeled the
   whole scene (and the labels showed ORANGE/untracked on a solo boot -- registry
   Element minting appears session-gated on solo; benign here, note for the overlay).
4. Cleanup: test instance killed, s_cooptest.sav deleted.

IMPLICATIONS: with the client loading the host's save, every key MATCHES -> the
connect snapshot becomes a thin all-exact-key true-up (no white-cube spawns, ~zero
fuzzy, sweep finds ~0, ingest balloon mostly gone -- the 1600-mirror spawn burst
was the divergence cost). Existing harness machinery reusable: LoadStorySave +
VOTVCOOP_SAVE (slot boot), save_guard.cpp (SaveGames file ops + coop_backup),
order_sync chunking precedent (wire), host_save_picker/save_browser (slot list RE).

## NO-STEAL CONSTRAINT (user requirement) -- design stack

Honest limit: bytes that reach a client's machine can be copied by a determined
user; this is hygiene + deterrence, not DRM. Stack:
1. Ephemeral slot under a name the game's save MENU does not list (verify the list
   builder's prefix filter (s_/b_) vs the loader's slot-name flexibility -- if the
   loader takes any name, use a non-prefixed name like `zcoop_session`).
2. DELETE the slot file IMMEDIATELY after the world is up (the loaded state lives
   in memory; keep the blob in the DLL to re-write just-in-time if the game ever
   re-reads the slot, e.g. death/reload). On-disk window ~= the load duration.
3. Failsafe deletes: at disconnect AND at DLL boot (crash-proof stale cleanup).
4. Stage 2: client-side PRE-cancel of the game's save verb during a session
   (interceptor precedent) -- a coop client must not persist host state anyway.
5. data.sav (global progression) is never transferred.

## STALENESS NOTE

The on-disk host save lags live state. Stage 1 may ship on-disk-save + the existing
snapshot true-up (all-exact-key now); the host-side programmatic SAVE trigger at
client-join ("save on join") closes the gap properly -- needs the save-verb RE
(workflow grounding covers it).

## SAVE-ON-JOIN REFINEMENT (user constraint, 2026-06-10): the game BLOCKS saving during events

USER: the developer turns saving OFF at certain gameplay moments (events); force-saving
there could corrupt the save/session. Design response (locked):
1. NEVER bypass the developer gate. The host-side trigger calls ONLY the game's own
   top-level save entry point (the same path the save menu / sleep flow uses, which
   contains the dev's own guards) -- never low-level serialize/write internals.
2. RE must establish WHERE the gate lives: UI-gated (the menu button merely disabled
   -> we must read the gate FLAG ourselves and not call when blocked) vs
   function-gated (saveGame() itself no-ops when blocked -> calling is always safe).
   Probe item: bp_reflect the save menu widget / sleep flow for the disable condition.
3. ATTEMPT-save semantics with OBSERVED verification: trigger -> wait ~2s -> check the
   slot file actually changed (mtime/size). Gate-agnostic: if the game refused, the
   file is unchanged.
4. GRACEFUL FALLBACK: blocked or failed save -> transfer the LATEST ON-DISK save
   immediately. Staleness is a PERF cost, not a correctness cost: the existing
   true-up (exact-key converge / sweep destroy / mirror spawn) closes ANY gap --
   worst case approaches today's smoke-proven behavior. The client never waits on a
   host event to end.
5. Debounce: one save attempt per join; concurrent joiners share one baseline.
   All UFunction calls via the game-thread pump as usual.
(VOTV autosaves on sleep, so the on-disk fallback is usually fresh anyway.)

## WORKFLOW VERDICT (connect-world-bootstrap-design, 9 agents, ~28 min) + STAGE-1 STATE

WINNER: SAVE-TRANSFER-AT-JOIN (hybrid) -- all 7 breaker blockers ABSORBED. Full verdict JSON:
C:\Users\Alexgrv\AppData\Local\Temp\claude\d--Projects-Programming-VOTV-MP\86866d94-8692-44e2-b0a3-136eb16f4d11\tasks\w2t42d671.output
Key absorptions changing the build: B1 autosave() is a TIMER-ARM NO-OP -- the real host
trigger is mainGamemode::save(quicksave, bypassEvent=TRUE, overwriteSubsave) (gate-skip
disasm pinned in the verdict; stage-0 item) + TORN-READ guard mandatory (VOTV writes .sav
in place: trust only size+mtime stable across >=2 polls + double-read CRC equal). B2 the
MTA invariant: HOST-side per-slot worldReady gate on world-mutating reliable kinds
(allowlist IsPreWorldSendableKind) + client belt-and-braces discard + net_pump
world-dependent tick blocks (puppet pose-apply!) gated on the world stamp. B3 WorldReady
predicate = in-gameplay AND HasSeededOnce AND IsRegistrySeededForCurrentWorld AND
!InPurgeEpisode AND a SeedGeneration bump after gameplay-detect. B4 slot = zcoop_<pid>
(menu-invisible prefix, per-instance; boot sweep age>1h only). B5 NO delete-after-load
(subsave re-reads!) -- file lives for the session; delete at disconnect + boot sweep.
B6 player scrub post-load: saveSlot reset_player_* verbs (saveSlot.hpp:113-133) --
inventory/equipment/hand only for stage 2 (position scrub risks the origin hang);
upgrades@0x48 joins the scrub; emails/Task SHARED by design. B7 engine.cpp caches
g_storySave ONCE -- engine::ResetCachedSave() required before a second in-process load
(rejoin); two-slot load probe before cutover.

### STAGE 1 SHIPPED THIS SESSION (build-clean, DORMANT -- zero behavior change)
- protocol.h: v56; kinds SaveTransferRequest=42 / SaveTransferBegin=43 / SaveTransferChunk=44 /
  ClientWorldReady=45; SaveTransferBeginPayload{totalBytes,chunkCount,crc32,gameMode,pad3}=16B;
  kSaveChunkBytes=56K (DIVERGES from the verdict's 220B chunks: our session bulk path ships
  ~308 GNS messages instead of ~78k -- the sink work below made it cheap; transferId SKIPPED:
  the v16 session epoch already rejects stale-generation packets).
- session_lanes.h: SaveTransferBegin/Chunk -> Lane::Bulk (phase-ordered before the bracket);
  IsPreWorldSendableKind allowlist (Join/AssignPeerSlot/PlayerJoined/SaveTransfer*/ClientWorldReady).
- session.h/.cpp: SetBulkSink (net-thread bulk diversion BEFORE the 228B inbox copy; chunk kind
  exempted from the kMaxReliablePayload send cap, quiet-fail on backpressure = the pacing signal);
  MarkSlotWorldReady/IsSlotWorldReady per-slot atomics (gate NOT yet armed in send paths -- wires
  with its callers, no half-armed drop of current flows).
- save_guard: SaveGamesDir() exported (shared resolver).
- NEW coop/save_transfer.{h,cpp} (~420 LOC): host per-slot stream (OnRequest arms; TickHost runs
  the torn-read stable-capture then 4x56K chunks/tick paced by send-failure; CancelForSlot),
  client assembler (net-thread BulkSink + game-thread OnBegin -> CRC -> temp+rename write of
  zcoop_<pid>.sav), states Idle/WaitingBegin/Receiving/ReadySlotWritten/NoSaveAvailable/Failed,
  CleanupStaleSlotsAtBoot (age>1h), OnDisconnect (reset + delete own slot). CMakeLists entry.

### STAGE 2 WIRING PLAN (next session -- exact seams, all grounded this session)
1. net_pump.cpp: (a) host connect edge (line ~600 block): REPLACE TriggerForSlot+broadcasts with
   nothing (save flow starts on the client's Request); EXTRACT the whole replay block into
   RunConnectReplayForSlot(slot) fired by ClientWorldReady; (b) host Tick: save_transfer::TickHost();
   (c) client tick: ClientNoteConnected() on the slot-0 connect edge; WorldReady sender with the B3
   predicate + g_worldReadySent latch (reset on disconnect) + session.MarkSlotWorldReady wiring +
   ARM the send-path gate in session.cpp (6 lines, both send paths); (d) disconnect edges:
   save_transfer::CancelForSlot / OnDisconnect + MarkSlotWorldReady(slot,false); (e) gate the
   client puppet pose-apply block on world-announced (menu-window crash risk).
2. event_feed.cpp: cases SaveTransferRequest -> OnRequest(senderSlot) [host], SaveTransferBegin ->
   OnBegin [client], ClientWorldReady -> MarkSlotWorldReady + RunConnectReplayForSlot [host];
   client pre-world discard gate (allowlist mirror of IsPreWorldSendableKind; count + one log).
3. ue_wrap/engine: LoadStorySave(slot, forceGameMode=-1) param threaded to ApplyGameModeFromSlot
   (zcoop_ prefix can't prefix-match; pass save_transfer::ReceivedGameMode()); ResetCachedSave()
   (clear g_storySave + g_gameModeApplied) called before the coop-slot load.
4. harness.cpp: menu-mode client join branch (line ~447): ClientArm() -> StartCoopSession FIRST ->
   poll GetClientState (timeout ~120s -> join_progress::Fail): ReadySlotWritten -> ResetCachedSave +
   BootStorySaveBlocking(slotOverride=CoopSlotName(), forceGameMode) ; NoSaveAvailable/Failed ->
   BootStorySaveBlocking(forceFresh=true) fallback (= today). SetHostSlot at the 3 load sites
   (BootStorySaveBlocking success slotA, DriveHostBootIfPending b->slot, CreateNamedSave name).
   CleanupStaleSlotsAtBoot + save_transfer::Install(&g_session) at boot.
5. join_progress/loading_screen: Downloading phase (GetProgress MB bar). Sweep-before-Complete
   cover reorder + Finalizing settle (verdict stage 3).
6. Stage-0 probes still open (before the cutover smoke): zcoop_ slot-name LOAD probe (zero-code,
   copy s_1234 -> zcoop_test + VOTVCOOP_SAVE=zcoop_test + forceGameMode), two-slot rejoin probe,
   bp_reflect saveSlot.json (reset_player scope, saveToSlot subsave conditions), ui_saveSlots
   prefix-filter verify, save(bypassEvent) disasm pin.
7. Smoke: env-flow regression (fresh client still works -- WorldReady fires immediately) + the
   menu-mode autonomous repro (env VOTVCOOP_MENU_CONNECT=1 -> session_manager::ConnectDirect at
   the menu) + 2-same-machine-clients zcoop collision case. THEN deploy + hands-on.

### STAGE-2 PROGRESS (same session, post-verdict): WORLDREADY CUTOVER SHIPPED build-clean
Wired LIVE: net_pump connect-edge replay EXTRACTED to RunConnectReplayForSlot (net_pump.h
export) fired by event_feed ClientWorldReady (host marks MarkSlotWorldReady too);
client WorldReady sender in Tick (B3 predicate: Local + HasSeededOnce +
IsRegistrySeededForCurrentWorld + !InPurgeEpisode; once per connection, reset on
disconnect); save_transfer::TickHost in host Tick; per-slot disconnect edge: save_transfer::
CancelForSlot + MarkSlotWorldReady(false); event_feed cases SaveTransferRequest/Begin/
ClientWorldReady; engine LoadStorySave(slot, forceGameMode=-1) + ApplyGameModeFromSlot
force path + ResetCachedSave(); harness: save_transfer::Install(&g_session) +
CleanupStaleSlotsAtBoot at boot. STILL DORMANT: the transfer itself (nothing calls
ClientArm -- menu-mode browser join still fresh-boots; the harness rewire at line ~447
is the remaining cutover step) + the send-path world-gate arming + client pre-world
discard gate + harness SetHostSlot at the 3 load sites + join_progress Downloading
phase + B6 scrub. Deployed all-4 hash-match; smoke launched (env flow regression:
client in-world -> WorldReady fires within a tick -> replay timing unchanged).

### STAGE-2 COMPLETE (same session, ~18:50): FULL v56 SAVE-TRANSFER FLOW WIRED + B2 GATES ARMED
First smoke (WorldReady cutover only): PASS with perfect evidence -- host "awaiting
ClientWorldReady" at the edge, client announced 2 s later (B3 predicate held), replay fired
on the announce, sweep 4255/3049/1019 (run-5 signature), plateau 10.73GB.
Then wired + build-clean + deployed all-4 hash-match:
- harness: BootStorySaveBlocking(forceFresh, slotOverride, forceGameMode) -- override loads
  the zcoop slot; non-fresh env/ini loads call save_transfer::SetHostSlot (the host slot
  source); DriveHostBootIfPending sets it for picker-hosted worlds.
- harness DriveMenuModeJoinWorldBoot(): the menu-mode join body -- ClientArm ->
  StartCoopSession AT THE MENU -> poll GetClientState (drains Cancel/cover-loss/dead-session
  itself, mirroring the RunPlayLoop abort branch; 120 s timeout) -> ReadySlotWritten:
  ResetCachedSave + load zcoop_<pid> with the wire GameMode; NoSave/Failed/timeout:
  fresh-boot fallback (= pre-v56 baseline).
- B2 GATES ARMED: session.cpp SendReliableToSlot + the SendReliable fan-out loop +
  session_relay BOTH relay paths (unreliable pose relay: IsSlotWorldReady only; reliable
  relay: + IsPreWorldSendableKind) now drop world-mutating traffic to pre-world slots.
  Client marks slot 0 (host) world-ready at its connect edge. net_pump client puppet
  spawn gated on g_worldReadyAnnounced (file-scope atomic; no engine spawns into the
  menu world from stray poses).
DEFERRED (stage 3): client-side event_feed pre-world discard (belt-and-braces -- the
host-side gate covers the live paths), join_progress Downloading phase + MaybeTimeout
check vs the download window (the 120 s harness timeout is the effective failsafe; verify
the cover timeout is not shorter), B6 player scrub (reset_player_inventory/equipment/hand),
host attempt-save-on-join (needs the save(bypassEvent) stage-0 pin), the menu-mode
autonomous smoke env (VOTVCOOP_MENU_CONNECT). Second smoke (full build, env flow)
launched -- verdict to be read; menu-mode path is HANDS-ON-testable after it passes
(browser join; any failure falls back to the fresh-world baseline).

### FULL-BUILD SMOKE (env flow): FAIL -- the PRE-EXISTING fresh-world ingest balloon, NOT the new code
The v56 mechanics all worked (client log: AssignPeerSlot -> ClientWorldReady announced ->
replay ran -> exact-key binds streaming). The client then ballooned through the ENV-flow
adoption (fresh world + ~3000 mirrors): 4.2GB(t=53) -> 9.4(t=64) -> 10.5(t=70) -> dropped
to 7.9 w/ title lost (t=75) -> dead (t=80). Last client frames: netPumpTick=49.3 ms/fr,
interactable=17.3, reaper=10.7 -- game thread drowning during the burst. Same marginal
balloon as the 16:40 cap-kill (cutover smoke passed the identical flow at 10.73GB plateau
minutes earlier -- it sits AT the edge; variance decides). The env smoke exercises exactly
the flow save-transfer REPLACES; the menu-mode path (which avoids the doubled world +
mirror burst) is the fix and needs its own autonomous repro (VOTVCOOP_MENU_CONNECT) --
next session, before any hands-on verdict on the balloon.
ALSO (user req): object_overlay=0 in all inis -- overlay is F1-menu-only now; smokes flip
the ini per-run when they need labels.

### UNIFIED JOIN (user mandate ~19:10: ".bat files go the server-browser way automagically")
The env/.bat CLIENT flow now IS the save-transfer join -- RULE 2: the fresh-boot client
path survives only as the in-function fallback (no save / failed / timeout). harness play
branch: net config read FIRST; saveTransferClient skips the world pre-boot; the autotest
teleport block extracted to a shared runAutotestTeleport lambda (host/solo: before Start;
client: after the world exists); client arm = BeginConnect(peerIp) cover [regression-A
browser-only rule deliberately retired -- the env client IS the browser flow now] +
ClientArm + StartCoopSession + DriveMenuModeJoinWorldBoot. join_progress kMaxJoinMs
90s -> 240s (the cover now legitimately spans download+load; the harness 120s transfer
timeout stays the primary failsafe). object_overlay=0 in all inis (user req: F1-only).
Deployed all-4 hash-match; the END-TO-END save-transfer smoke (the first true exercise
of the whole path) launched -- THE expected win: client loads ONE world from the host
save (host-like ~4GB RSS, no doubled-world balloon, ~all exact-key binds, ~0 destroys).
mp.py untouched (VOTVCOOP_FRESH now unused by clients; fallback uses forceFresh directly).

### DEADLOCK FOUND + FIXED (first end-to-end attempt, ~19:00) + FULL UNIFICATION (user directive)
SMOKE FAIL forensics: client ARMED + connected (in=60 pkt/s) but out=0, no Request ever,
host never saw it -- "net: peer slot 0 CONNECTED" logged but net_pump's connect edge never
ran. ROOT CAUSE: DriveMenuModeJoinWorldBoot blocks the TimelineThread -- the SAME thread
whose loop posts net_pump::Tick -- and the entire transfer (connect edge -> Request,
event_feed -> Begin, TickHost -> chunks) lives in that tick. Latent in the browser path
too (never exercised autonomously). FIX: the wait loop now posts the RunPlayLoop composite
itself (Tick + nameplate + overlay + chat + shutdown hooks, ~60 Hz).
USER DIRECTIVE ("client stuck at connecting... actually host via master with HIDDEN
flag"): (1) env CLIENT now queues session_manager::ConnectDirect("ip:port") and enters
RunPlayLoop(idleInGameplay=false) -- the browser TakePendingStart menu-mode branch is THE
ONE join path (RULE 2: the harness play-branch special-case join deleted same-day);
autotest-positioning teleport no longer runs for env clients (puppetshot/ragdollshot need
a post-join hook when next used). (2) NEW session_manager::AnnounceEnvHostHidden: env host
= REAL master-announced lobby, immediately SetListed(false) (brief listed window noted;
announce-time hidden flag = master-API addition later); master down -> direct-only,
hosting unaffected. Harness host arm calls it post-StartCoopSession with the env/ini slot
as the world label. Build clean, deployed all-4 hash-match, end-to-end smoke re-running.

### END-TO-END SMOKE: PASS -- ARCHITECTURE PROVEN; ONE LOCALIZED REGRESSION LEFT (menu-window pump churn)
THE WINS (first complete exercise): Request 19:08:51 -> Begin 17,226,903 B / 301 chunks ->
written zcoop_21704.sav CRC-OK 19:08:57 (**~5 s for 17 MB**) -> slot loaded -> world up
19:09:14 -> ClientWorldReady -> bracket = THE THIN TRUE-UP AS DESIGNED: **2035 already-
aligned exact-key binds + 240 drift-converges + only 26 new spawns** (vs 1600+723 spawns/
1019 destroys pre-transfer). 871 keyless piles seeded + expressed (the pile destroy+
respawn churn remains -- the position-bind optimization is the queued fix). Both peers
stable to t=150, exit 0.
THE REGRESSION (localized by the curve): client RSS 3.1GB(t=16) -> 11.05 peak(t=43) ->
10.92 plateau -- ALL BEFORE the Request (19:08:51). Cause: the deadlock fix pumps the FULL
net_pump::Tick at 60 Hz AT THE MENU for ~35-40 s; with no gameplay world, every subsystem
ensure-install retries EVERY tick (their BP classes only exist post-gameplay-load) = full
GUObjectArray walks w/ per-entry allocations, 60 Hz x N subsystems -> allocator-pool bloat
(plateau, never returned) + game-thread ticks >100 ms -> post-queue backlog (explains the
~35 s connect->Request lag). EXACTLY the workflow B2 "menu-pump incident" line item.
FIX (next pass, B2's own design): gate net_pump::Tick's gameplay-only sections (ensure-
installs, reapers, watches, interactable TickConnect, perf scopes) on a world-up check
(players Registry Local() != null); the menu window keeps ONLY: connect edges, event_feed
drain, save_transfer Tick/edges, join_progress. Cheaper alternative REJECTED (throttling
the pump = crutch; the walks would still churn).
ALSO queued: keyless-pile position-bind (the remaining destroy+respawn class -- sound now
that worlds are save-aligned); B6 inventory scrub; host attempt-save-on-join; download bar.
HANDS-ON: usable NOW (stable, fallbacks proven) -- caveat: the client spends ~40 s near
~11 GB during the join until the menu-pump gate lands (risky on 16 GB boxes).

---

# ===== SESSION END STATE (compact-prep, 2026-06-10 ~19:30) =====

## DEPLOYED BUILD (all 4 folders hash-match) = the END-TO-END-PASS build
Protocol **v56**, save-transfer join COMPLETE + PROVEN: .bat/env client AND browser take
ONE path (session_manager ConnectDirect/JoinLobby -> TakePendingStart menu-mode branch ->
ClientArm -> connect at menu -> Request -> 17MB/301-chunk download (~5 s, CRC) ->
zcoop_<pid>.sav -> ResetCachedSave + LoadStorySave(forceGameMode) -> ClientWorldReady
(B3 predicate) -> host replay = THIN TRUE-UP (measured: 2035 aligned / 240 converged /
26 spawns). Env host = LanDirect listen + AnnounceEnvHostHidden (master announce ->
immediate SetListed(false)). Fallbacks proven (no-save/fail/timeout -> fresh boot).
B2 gates armed on all 4 send paths + client puppet-spawn gate. No-steal: zcoop_<pid>
menu-invisible, session-lifetime, deleted at disconnect + boot age-sweep (>1h).
object_overlay=0 in inis (F1-only; smokes flip per-run).

## THE ONE KNOWN REGRESSION (fix-first next session)
Menu-window pump churn: DriveMenuModeJoinWorldBoot's 60 Hz full net_pump::Tick at the
MENU -> un-latched subsystem ensure-installs walk GUObjectArray every tick (classes only
exist post-gameplay) -> client RSS 3.1->11.0GB in ~25 s BEFORE the Request + ~35 s
connect->Request lag (post-queue backlog). FIX = world-up gate (players Registry
Local()!=null) on Tick's gameplay-only sections; menu window keeps connect edges +
event_feed + save_transfer + join_progress only. Expected result: ~4GB client joins +
~1 s Request. (The workflow B2 "menu-pump incident" line item, manifested.)

## SESSION-6-EVENING FILE INVENTORY (save-transfer arc; ALL UNCOMMITTED)
NEW: coop/save_transfer.{h,cpp}; coop/dev/object_overlay.{h,cpp} (earlier arc).
protocol.h (v56, kinds 42-45, SaveTransferBeginPayload, kSaveChunkBytes, history);
session.h/.cpp (bulk sink, 65000 cap exemption, quiet chunk-fail, world-ready flags,
B2 gates x2); session_lanes.h (lanes + IsPreWorldSendableKind); session_relay.cpp
(B2 gates x2); net_pump.h/.cpp (RunConnectReplayForSlot extract, WorldReady sender,
TickHost, disconnect resets, puppet gate, g_worldReadyAnnounced); event_feed.cpp
(3 cases + includes); join_progress.cpp (kMaxJoinMs 240s); save_guard.h/.cpp
(SaveGamesDir export); engine.h/.cpp (LoadStorySave forceGameMode, ResetCachedSave);
session_manager.h/.cpp (AnnounceEnvHostHidden); harness.cpp (BootStorySaveBlocking
slotOverride+forceGameMode+SetHostSlot, DriveMenuModeJoinWorldBoot w/ pump, play-branch
restructure: client=ConnectDirect + RunPlayLoop(!stc), host=hidden announce, Install+
boot-sweep, save_transfer include); CMakeLists (+2 files).

## POST-COMPACT PICKUP ORDER
1. MEMORY.md top entry -> 2. memory/project_snapshot_adoption_2026-06-10.md ->
3. THIS doc bottom-up (SESSION END STATE -> the dated sections above it).

## NEXT-SESSION ORDER
1. **World-up gate in net_pump::Tick** (the menu-pump fix above) + re-smoke: expect
   client ~4GB join, Request <2 s. 2. Keyless-pile position-bind (870 destroy+respawn ->
   in-place binds; sound now worlds are save-aligned). 3. B6 player scrub
   (reset_player_inventory/equipment/hand post-load; saveSlot.hpp:113-133). 4. Host
   attempt-save-on-join (save(quicksave,bypassEvent=TRUE) + observed-verify + stale
   fallback). 5. join_progress Downloading phase (GetProgress bar). 6. Stage-0 probes
   (rejoin two-slot, bp_reflect saveSlot.json). 7. perf_probe=0 pre-ship; extraction
   backlog (remote_prop_spawn 819); harness PID-tracker.
HANDS-ON: usable NOW (stable, fallbacks proven); caveat ~11GB client transient during
the join until fix #1 lands.

===== CLIENT RAM BALLOON — ROOT CAUSE + FIX (2026-06-10 ~20:00, post-compact) =====

USER (verbatim intent): "fix the balooning on client for fucks sake it's been many
sessions where it just baloons while you ignored it." Correct call: every prior
balloon (19 GB Install loop, quadbike-seated 5 fps + RAM, death-menu churn,
host-quit-to-menu churn, the v56 menu-window 3.1->11.0 GB) was patched AT ITS
CALL SITE (a latch here, a gate there) while the underlying engine stayed
balloon-capable. This pass fixed the CLASS.

THE THREE COMPOUNDING ROOT CAUSES (all proven in code, none guessed):
1. reflection::ToString constructs a std::wstring PER CALL (plus the engine
   FString render), and EVERY GUObjectArray walk helper (FindObjectByClass,
   FindObjectsByClass, CountObjectsByClass, FindObject, FindClass, FindFunction,
   players_registry::RescanLocal) called it (via ClassNameOf) PER UOBJECT
   SCANNED -- ~250k allocations per walk, ~500k counting the engine-side
   FString. The codebase's own comment (players_registry.cpp, quadbike RE
   2026-06-08) names this "the same wstring-bomb shape as the 19 GB Install
   regression" -- the pattern was DOCUMENTED but never fixed at the source.
2. players::Registry::Local() cache-misses DETERMINISTICALLY while no gameplay
   world is up (the v56 menu-mode join window), and was called per tick at
   60 Hz from net_pump::Tick (twice: the world-ready announce check + the
   g_netLocal resolve) AND nameplate::Update (unconditionally at the top).
   3 full allocating walks per 16 ms tick = the 3.1->11.0 GB pre-Request climb.
3. The TimelineThread posts the pump composite at 60 Hz UNCONDITIONALLY; with
   each composite taking ~100+ ms (the walks), the game thread drained ~7/s
   while 60/s queued -> unbounded posted-task backlog = the 35 s
   connect->Request lag (the Request lives in a queued tick).

THE FIX (4 layers, all root-cause-class, no per-caller crutches):
1. ue_wrap/reflection: RenderNameToScratch (the reused per-thread scratch,
   extracted from ToString) + NameEquals/NameStartsWith/NameContains --
   allocation-free compares with semantics IDENTICAL to `ToString(x) == y`.
   ALL walk helpers converted. By-class walkers additionally walk by CACHED
   CLASS POINTER: g_classCache (FNV-1a keyed, full-string verified, mutexed)
   primed on the first textual match DURING a normal walk, revalidated once
   per walk (IsLive THEN NameEquals -- the && short-circuit is the UAF guard).
   Steady-state walk = N pointer compares, zero renders, zero allocations.
2. players_registry: RescanLocal walks by cached class ptr (static, same
   revalidation); Local() adds a 500 ms NEGATIVE-result TTL (localMissAtMs_)
   covering both the cold-menu miss and the dead-cache mid-travel window;
   InvalidateLocal resets it. Worst case world-up detection is 500 ms late --
   nothing against a multi-second world load.
3. net_pump::Tick: hoists localNow=Local() ONCE per tick; gates the gameplay-
   only sections on worldUp (snapshot drain, the entire subsystem Tick chain,
   dev probes, puppet drive + ragdoll + pose-diag + remote_prop::Tick). The
   menu window keeps: TickHost, ElementDeleter flush, the reaper (self-gated,
   owns quit-to-menu flee), connect/disconnect edges, the world-ready announce,
   the connect-failure edge, event_feed::Update (delivers SaveTransferBegin).
   nameplate::Update resolves Local() lazily (only when a live puppet exists).
   The reaper's own per-4s ToString world-name check converted to NameContains
   (audit CRITICAL-1: the one surviving allocation in the touched file).
4. harness: PostPumpComposite -- coalesces the 60 Hz composite posts (skip
   while the previous composite hasn't run; 500 ms timestamp self-heal so a
   dropped/bypassed composite can't wedge the pump). Applied to all three
   composite posters (RunPlayLoop, DriveMenuModeJoinWorldBoot wait loop,
   netloopback). Bounds the backlog to ~1.

SMOKE PROOF (mp.py smoke, 180 s, 3 s sampling, exit 0 PASS):
- Client RSS through the ENTIRE menu+connect+download window: FLAT ~3.8 GB
  (regression run: 3.1->11.0 GB climbing).
- connect->SaveTransferRequest: ~1 s (was ~35 s). 17,226,903 B / 301 chunks
  downloaded in ~5 s, CRC ok; zcoop load; ClientWorldReady at +9 s from arm.
- Client in-world plateau: ~6.85-6.97 GB over 140 s (drift ~0.9 MB/s, same
  slope as the HOST's own 3.92->4.01 GB -- the game's normal churn, not ours).
  vs the old flow's ~10.7-11 GB plateaus. Headroom under the 12 GB cap: ~5 GB
  (was <1 GB -- the "client balloons and crashes" kills).
- Host steady ~3.9-4.0 GB; FPS both ~108-118; pose trail 0 cm; ping 0 ms.
- Logs: ZERO [Error]/Fatal/SEH/IsLive-AV on both peers (the grep's 13/11
  "FAULT" hits were case-insensitive matches of the word "Default" in widget
  lines). Non-perf WARNs all pre-existing patterns: the 870 keyless-pile
  setKey skips (the queued position-bind work), the host's time/sky sends
  correctly BLOCKED by the B2 gate during the 8 s pre-world-ready window
  (stop exactly at world-ready), one snapshot DEFER that resolved into the
  clean 3175-candidate drain at +20 s.
- Host perf probe: netPumpTick 0.24-0.98 ms/fr, installObs 0.00, detour self
  ~165 ns/dispatch -- host steady-state untouched by the gate.

AUDITS (2 agents, both on the diff):
- Correctness: ZERO new criticals. Brace structure of the two worldUp blocks
  verified line-by-line; RescanLocal preserves CDO+IsLive-before-GetController;
  no data races (g_pumpPostedAtMs atomic; registry fields game-thread-only,
  no off-thread Local() caller found); the BeginClassWalk TOCTOU clear-guard
  (`it->second.cls == cls`) proven sound; the inner g_worldReadyAnnounced
  puppet gate confirmed LOAD-BEARING (guards worldUp-true-but-replay-not-
  landed -- NOT dead code; complementary to the outer gate, kept).
- Perf/hot-path: full table produced, all rows OK after fixes; CRITICAL-1
  (reaper ToString) FIXED with NameContains; WARN-1 (netloopback bare Post)
  FIXED with PostPumpComposite; NOTE-1 comment added (short-circuit UAF guard).
  DEFERRED (cold paths, cleanup debt, written here per the audit): FindClass
  still allocates 1-2 wstrings per walk for name-MATCHED objects only (needs
  a NameEndsWith or scratch suffix check); InvalidateLocal has NO callers
  (pre-existing; either wire it at the level-travel hook or delete + fix the
  header comment); sMpClass and g_classCache hold redundant mainPlayer_C
  entries (both independently revalidated -- harmless).
- File sizes: reflection.cpp 643, players_registry.cpp 399, nameplate.cpp 137
  (all under soft cap); net_pump.cpp ~1259 and harness.cpp ~1079 BOTH
  PRE-EXISTING past the 800 soft cap (this change +~30/+~28 lines; extraction
  proposals stand: net_pump's subsystem-Tick chain; harness's scenario bodies).

WHAT THIS MEANS FOR THE OLD BALLOONS: the per-site patches (Install latches,
the death-flee bypass, the quit-to-menu flee, the quadbike cache-validation
fix) all stay -- they fixed real per-site logic. But the AMPLIFIER under all
of them (any cold/no-world window + any per-tick caller = gigabytes) is gone:
a cold walk now costs pointer compares, a deterministic miss is TTL'd, the
menu window runs only what the menu window needs, and the post queue is
bounded. perf_probe remains ON in the inis for the next hands-on; turn off
pre-ship.

===== KEYLESS-PILE POSITION-BIND (2026-06-10 ~20:35) =====

The last destroy+respawn class closed. Pre-change: the host eid-expresses ~870
keyless actorChipPile_C piles per bracket; the client (whose eidOnly lane
skipped local matching -- correct back when worlds were per-peer RNG) fresh-
spawned 870 mirrors while the sweep destroyed its own 870 save-loaded piles.
Post-save-transfer that churn was pure waste: both worlds load THE SAME SAVE.

MECHANISM (remote_prop_spawn.cpp): bracket-scoped candidate index (one
GUObjectArray walk on the FIRST keyless-pile expression of a bracket: chipPile
lineage + live + non-CDO + unclaimed -> {actor, pos, chipType}); the bind lane
runs on eidOnly expressions while the bracket is open, AFTER the eid-registry
dedup miss, BEFORE the keyed fuzzy block: nearest candidate within 30 cm with
equal chipType + EXACT wire class (NameEquals) + still-unclaimed + IsLive ->
swap-pop, RecordClaim, ReconcileToHostPhysics, 2 cm epsilon-gated converge,
RegisterPropMirror(eid), WatchPile(eid) (identity death-watch -- a local grab
morph-destroy still broadcasts PropDestroy(eid) exactly like a fresh mirror).
No match -> the fresh-spawn mirror lane (a pile the host created AFTER its
save has no local twin -- the user's question, answered by construction: the
snapshot expresses the host's LIVE registry, so post-save piles fresh-spawn
and pre-save piles the host lost stay unclaimed -> swept). `fromConvert=true`
(OnConvert's synthesized pile spawn) bypasses the bind lane -- a convert-born
pile has no local counterpart and must never bind a nearby unrelated pile.
Index reset at BeginClaimTracking / sweep end / ResetClaimTracking (dangling-
pointer hygiene vs the post-sweep ForceGarbageCollection). No protocol change
(v56; the lane is receiver-side only).

SMOKE PROOF (150 s, PASS, exit 0):
- "pile-bind index built -- 870 local chipPile candidate(s)"; binds #1..#800+
  ALL at d=0.0 cm "aligned" -- the save-transfer worlds are sub-mm identical.
- Claim sweep: 3338 in-universe / 3092 claimed / **83 destroyed** (was ~1018):
  81 trashBitsPile_C (host collected them since its save -- correct adoption)
  + 1 chair + 1 swinger. ZERO chipPiles destroyed.
- The 878-line "StringToFName('') setKey skipped" spam: **0 lines**.
- CLIENT PLATEAU ~3.89 GB == HOST 3.94 GB (was 6.9 GB): killing the 870
  destroy+respawn churn also killed ~3 GB of bracket transient heap growth.
  Join memory is now host-parity -- the ideal end state.
- 0 errors/Fatals both logs.

FILE SIZE: remote_prop_spawn.cpp 819 -> 958 LOC (soft cap 800, hard 1500).
Pre-existing offender, now further past. STANDING EXTRACTION PROPOSAL: split
the claim-tracking + sweep + pile-bind-index complex (~300 LOC, one concept:
"adoption bracket bookkeeping") into coop/adoption_sweep.{h,cpp}; OnSpawn's
bind lanes stay (they ARE OnSpawn). Do as its own commit next refactor pass.

--- pile-bind AUDIT + fixes (same evening, ~20:45) ---
Agent review of the bind diff: 1 CRITICAL + 1 IMPORTANT + notes, ALL FIXED
same pass (re-built, re-smoked):
- CRITICAL-1 (dual identity): a BOUND pile is the client's own SEED-WALKED
  actor -- it carried a CLIENT-MINTED local eid in prop_element_tracker AND
  the host-eid mirror registration. A local grab morph-destroy would have the
  K2_DestroyActor PRE observer broadcast a STRAY PropDestroy(clientEid)
  alongside the death-watch's correct PropDestroy(hostEid). (In practice the
  stray eid is unknown to the host -> discarded WARN noise; the latent
  landmine is any future flow that registers client-range eids host-side.)
  FIX: the bind RETIRES the superseded local identity --
  prop_element_tracker::UnmarkKnownKeyedProp(pile) (drains the reverse map +
  frees the Element shadow, ABBA-safe) -> GetPropElementIdForActor reads
  kInvalidId -> the PRE observer stays silent (keyless+no-eid fall-through) ->
  the WATCH is the sole destroy speaker, the exact fresh-mirror invariant.
- IMPORTANT-2 (bracket-long raw pointers): index entries now capture
  InternalIndexOf at build; bind-time liveness is IsLiveByIndex (no deref of
  a possibly-GC-freed pointer mid-bracket) -- the WatchedPile pattern.
- NOTE-3: chipType-async assumption documented in-code (a lazy-set chipType
  would only cause a harmless miss -> fresh-spawn fallback, never a wrong
  bind).
- Finding 4 (file size): remote_prop_spawn.cpp ~981 LOC. Extraction proposal
  RECORDED (auditor concurs): coop/snapshot_adoption.{h,cpp} takes claim
  tracking + sweep + pile-bind index (~400 LOC, one concept: adoption-bracket
  bookkeeping). MUST land as its own commit BEFORE the next feature touches
  this file.
- Auditor verified: no double-bind (swap-pop + claimed-check), fromConvert
  plumb complete (only 2 callers, no fn-ptr use), perf clean (1 walk/bracket
  + ~756k float compares total, no allocs in the scan).

===== HANDS-ON ROUND 2 (2026-06-10 ~20:55) -- 4 issues, root causes + fixes =====
User: "client can't interact with the wall office objects; host picks up a wall
object -> dupes in client's world; door E stress -> open/close desync; garbage
container piles (E spawns item, usage 6/7) need mirror-sync." Logs preserved:
research/handson_logs_2026-06-10/evening_handson_2/{host,client}.log.

(1) CLIENT CAN'T GRAB PROPS -- FIXED + smoke PASS. Root cause (log-proven): the
join reconcile ran ReconcileToHostPhysics (SetSimulatePhysics(false)) on EVERY
bound prop BEFORE the epsilon check -> all aligned props kinematic -> VOTV's
PhysicsHandle grab refuses a non-simulating body. The log shows 183 E-presses
with 4 successful holds, and the wall became grabbable EXACTLY after the host's
grab+release re-enabled its physics. The P1 force-kinematic was the anti-storm
measure for teleport-converging into RNG-DIVERGENT layouts -- obsolete for
aligned props now that worlds are save-transferred. FIX (remote_prop_spawn):
ALIGNED -> no physics touch at all (the save-loaded copy already carries SP
physics: simulate-enabled + asleep = grabbable); DIVERGED -> kinematic, teleport,
then RestoreSpParityPhysicsAfterConverge (SetSimulatePhysics(NOT(static||frozen
||sleep)) -- the SP init formula; safe now: the converge target is the host's
rest pose in an IDENTICAL world); FRESH MIRRORS -> SpParitySimulate(physFlags)
instead of force-off. Smoke: PASS, client 3.92GB == host 3.94GB, perf clean --
no wake storm.

(2) THE WALL "DUPE" -- GHOST TWINS (forensics agent, log-proven; self-heal
SHIPPED, structural fix pending probe). The client world contains prop_C panels
that exist ON THE CLIENT ONLY: the world-load's LATE TAIL materialized +872
keyed actors AFTER the sweep ran (20:52:38 re-seed vs 20:52:36 sweep). At sweep
time they were KEYLESS (pre-Init) -> the "keyless non-pile: NEVER swept" rule
exempted them FOREVER -- but keyless is a TRANSIENT pre-Init state; their keys
minted later -> keyed, interactive, never-adjudicated ghosts in the wall stack.
User grabs a ghost -> host sees NOTHING (127x 'PropPose no local match' warns,
keys bYM_/qbvTV_ the host never had, eid=0) = one-way divergence. Host grabs
its bound panel -> client's bound copy flies to the puppet while the ghost
stays standing = the perceived dupe. The bound-panel pipeline itself round-
tripped correctly every time (<=12cm convergence). REFUTED: physics-divergence
(converged by key next hold), re-bracket (one bracket all session), wire dupes
(zero fresh prop_C spawns). The compounding bug: trash_collect's held-edge
predicate equated "has a key" with "peer already has it" -> the ghost was never
expressed even when CARRIED. FIXES SHIPPED: (a) held-edge predicate -> TRACKER-
KNOWN (keyed AND GetPropElementIdForActor!=invalid -> skip; untracked -> express
via PropSpawn with its key = the universal self-heal: anything a player touches
reaches the peer); (b) sweep probe: WARN histogram of keyless-skipped unclaimed
actors per class every bracket (quantifies the adoption-hole exposure per join).
STRUCTURAL FIX PENDING (per RULE 1 verify-first): twins-vs-extras is undecided
-- if the ghosts are a RE-RUN of the cubicle first-run gib spawner on the
client (suspected: matches the user's ORIGINAL "walls spawn in air and fall"
report), the fix is Fork-C-style client suppression of that spawner; the
spawner is not findable statically ('cubicleP' is a prop_C ROW NAME, not a
class -- bytecode string hunt needed). The probe histogram + next hands-on
decide. Candidates if late-tail-generic: deferred adjudication (keyless-at-
sweep actors re-tested when keys mint) or a load-quiescence gate on
ClientWorldReady (announce only after two zero-add re-seeds; costs ~8s join).

(3) DOOR E-STRESS DESYNC -- FIXED (architect agent verdict, MTA CObjectSync
shape). TWO compounding mechanisms: (a) the client's NATIVE BP toggle
(InpActEvt_use -> player_use -> doorOpen, ALL BP-internal = unobservable/
unsuppressable) toggled the LOCAL door on every E in parallel with our
DoorOpenRequest; (b) the host's corrective DoorState echo was SWALLOWED by
ApplyResolved's idempotent guard -- it compared against the LIVE engine field,
which the native press had already moved to the echoed value -> skip -> stale
lastKnown_ -> a persistent one-toggle-behind drift under rapid pressing.
FIXES (interactable_channel.h + interactable_sync.cpp): (a) the live-field
idempotent skip is now SYMMETRIC-ONLY -- HostAuth ALWAYS applies the echo (MTA:
the non-authority applies server state unconditionally, never trusting its own
simulation); (b) a PRE observer on InpActEvt_use clears the aimed door's
Active field (the BP's own CanOpen gate) for the body of the dispatch and the
POST observer restores it FIRST-thing -- the native chain no-ops, the host
echo is the door's sole state mover on the client. Pairing via a single GT
slot (g_useInputActiveCleared); every POST early-return path restores. The
seq-number N-peer hardening is deferred (2-peer scope; the reliable lane is
ordered, so always-apply is sufficient).

(4) trashBitsPile DISPENSER (E spawns item + uses 6/7 counter) -- RE agent
running; sync design + implementation next.

(4) trashBitsPile DISPENSER SYNC -- IMPLEMENTED (v57, TrashPileState=46). RE
verdict (bytecode-grounded): AtrashBitsPile_C : Aactor_save_C; the "6/7" =
amountA@0x0260 + amountB@0x0264 (int pair, displayed LIVE by lookAt's Format --
no refresh verb exists or is needed; raw writes fully consistent); THREE
writers, ALL BP-internal (E-grab playerGrabbed, prop_vacuum2 vacuumed,
prop_broom broomed) -> poll channel, not input observer (the doors sent=0
doctrine); depletion K2_DestroyActor's BP-INTERNALLY (invisible to the destroy
observer AND the next poll) -> proximity death-watch (8m, all writers act at
the player) -> the EXISTING keyed PropDestroy; counters SAVE-PERSISTED (mInt
rows) -> save-transfer joins start identical, only live decrements ride the
wire; rowless event-cluster piles BeginPlay-reroll per-peer -> the adopt
connect-snapshot trues up. The DISPENSED ITEM needs no new wire (corrected
assumption: its init() is ProcessInternal -- the Init-POST observer does NOT
fire; it mirrors via pickupObjectDirect -> the held-edge broadcast + pose
stream, already shipping). NEW coop/trash_pile_sync (~300 LOC, grime shape +
int-pair + destroy semantics -- deliberately NOT generalized into the Channel
until a 4th case); ue_wrap::prop Is/Read/WriteTrashPileAmounts; per-component
MIN merge for concurrent collects; deferred applies 25s TTL; depleted-key
replay in the connect snapshot; NotifyWireDestroy echo guard in event_feed's
PropDestroy case; relayable kind; keysHash log (~392 placed piles expected,
host==client is the smoke signal).

--- ROUND-2 AUDIT + fixes (~22:00) ---
2 CRITICAL + 1 IMPORTANT, all FIXED same pass + re-smoked:
- CRIT-1: trash_pile poll called GetActorLocation (a ProcessEvent dispatch!)
  per pile per 50ms = ~8000 PE/s through our own detour. Piles never move ->
  DELETED; the RebuildIndex-time position (2s) feeds the death-watch.
- CRIT-2: death-watch cover gap -- the claim sweep runs AFTER join_progress::
  Complete() (Idle) in the same drain; a swept near-camera trashBitsPile would
  re-broadcast as a depletion destroy -> the HOST K2-destroys a pile it
  legitimately has (data loss). FIX: the sweep's keyed branch calls
  trash_pile_sync::NotifyWireDestroy(key) for every doomed trashBitsPile
  (the same echo-guard shape as the wire-PropDestroy case).
- IMP-3: a BP-body SEH fault between the door PRE/POST observers would leak
  the cleared Active gate (door bricked). FIX: OnUseInputPre restores any
  leaked door first-thing (self-heals on the next E-press).
- Closed clean by the auditor: rebuild preserves poll baselines (no echo);
  death-watch sender erases before next tick (no loop); unkeyed-door PRE path
  leak-free; protocol/relay/payload all verified; senderSlot 0xFF adopt-denied.
- File sizes: event_feed.cpp 1358 (142 from HARD cap) -- extraction to
  coop/event_dispatch.cpp is DUE before the next ReliableKind; net_pump 1217,
  remote_prop_spawn ~1045 past soft cap pre-existing; trash_pile_sync 291 ok.

FINAL v57 SMOKE (pre-audit-fix run): PASS -- trash_pile keysHash IDENTICAL
host==client (0x3AF1EA4E36716ADC, 311 piles), connect-snapshot 311 delivered +
live adopt-applies on the client, door PRE+POST installed, pile-bind 870, the
ghost probe quantified the exposure (162 keyless skipped, top 109 prop_C --
the structural-fix dataset), 0 errors, client 3.9GB. Audit-fixed build
re-smoked after (see next entry if numbers differ).

===== SOUND BUGS (late evening, RE agent pass) =====
(1) "Remote clump throw no whoosh" -- NOT A SYNC BUG (hypothesis falsified by
bytecode+logs): the clump IS PhysicsHandle-carried at interp speed 100 (instant
tracking, no flick momentum), and every hands-on release was an E-key DROP --
a path with NO impulse (the LOCAL clump also fell at the feet; convert
transforms prove it). The real throw is LMB (traceThrow->throwShit ~1500cm/s,
velocity applied BEFORE grabbing_actor clears in one synchronous event -> our
edge sampler reads it correctly). E-drop silence is SP parity. Contingency
design (bounded 3-tick deferred release sample) documented in the agent
output; implement ONLY if a real LMB clump throw ever arrives slow on the wire.
(2) PUPPET FOOTSTEPS -- FIXED (smoke PASS, hands-on pending): the native
accumulator lives in mainPlayer's BP tick block @70172 (stride 150cm, |v|>10,
run x0.75 -> lib_C::step) and the puppet's BP tick is deliberately suppressed
(puppet.cpp SetActorTickEnabled(false) -- the SP-brain/camera-MPC guard);
Kerfur is audible via anim-notify notify_kerfurStep -> int_anim_events.step ->
the SAME lib_C::step (mainPlayer_C lacks that interface; adding = asset edit
A6). FIX: NEW coop/puppet_footsteps.h (header-only Stride: native constants,
teleport guard, idle re-prime) ticked from RemotePlayer::ApplyToEngine after
the CMC drive + NEW ue_wrap/votv_lib.{h,cpp} (latched lib_C CDO + step thunk,
args mirror the native call @70973). All sound selection/spatialization/
steppedOn reactions are the game's own. Cost: float math/frame + 1 PE per
150cm walked. Side effects flagged: observer-side stats.steps++ per remote
step (cosmetic); steppedOn fires on the observer (correct mirroring). Fresh
dumps: prop_garbageClump, lib, kerfurOmega, notify_kerfurStep (bp_reflection/).

===== HOST-GAME CONNECTION SELECTOR (2026-06-11, user req) =====
Picker gains Connection: AUTO (recommended; master P2P/ICE -- direct-or-TURN
automatic; TURN deliberately NOT a user choice) vs DIRECT (LanDirect listen on
net.port, NO master announce -- the master join flow hands P2P creds so a
listed direct lobby would mislead joiners; friends use Direct Connect) + a
Public-game checkbox (AUTO only; private = announce->SetListed(false), the
hidden shape). HostWithSave(+directConnection,+publicGame). DEFERRED w/ design:
port-open check needs a MASTER probe endpoint (master sends UDP probe ->
host session reports receipt -> "port closed, use AUTO" verdict); direct-lobby
LISTING needs master support (store/return host ip:port). Smoke PASS.

===== CONNECTION-TYPE SELECTOR + DIRECT LOBBIES (2026-06-11, user req) =====
3 connection types (TURN is NOT a user choice -- it's ICE's automatic fallback
INSIDE auto): AUTO (master P2P/ICE, always listed -- a relay game's only
rendezvous, hiding=unjoinable), DIRECT (LanDirect UDP listen on a forwarded
port, master-LISTED), LAN (.bat/manual Direct Connect). User visibility model:
AUTO can't hide at host (trap removed, RULE-2); hide via the IN-GAME scoreboard
once friends are in; DIRECT has a host-time "Hide from server browser" checkbox.

MASTER (coop_master_server.py): Lobby +conn/+direct_port; h_host parses
conn=direct + port (1024-65535) -- REJECTS out-of-range with 400 (audit R1: no
silent p2p downgrade that would hand joiners ICE creds a LanDirect host can't
speak) + REFLECTS accepted conn back; direct hosts get NO ICE/TURN block;
h_join direct -> {conn,addr=src-ip:port} (no creds); rows +conn (no direct_port
leak). master_smoke +8 direct checks, all PASS.

CLIENT: lobby_announcer.Host(+directPort); session_manager HostWithSave(+direct
,+hideFromBrowser) one worker announces P2P or DIRECT, listed=ok&&!(direct&&
hide); JoinLobby direct branch -> LanDirect dial; LobbyRow.direct + JoinInfo
{direct,addr}; g_listedState/ListedState()/HostListenPort(). Picker: AUTO
checkbox GONE, DIRECT 'Hide from server browser' checkbox. Scoreboard: host-only
'Show in server browser' toggle -> SetListed.

AUDITS (2 master + 1 client agent):
- **CRITICAL C-1 (master, the headline)**: resolve_client_ip took the LEFTMOST
  X-Forwarded-For = CLIENT-FORGED value. Behind the production xray proxy (peer
  =loopback -> XFF trusted), an attacker could forge XFF:<victim> and make the
  master (a) advertise a direct lobby's addr as the victim's, (b) [if portcheck
  shipped] UDP-probe the victim. FIX: take the RIGHTMOST XFF entry (what OUR
  trusted proxy appended = the real observed peer); X-Real-IP (proxy-overwritten)
  still preferred. The 1st audit's "lo.ip unpoisonable" PASS only held for the
  direct-exposure model; the proxy model is production -> 2nd audit caught it.
- **HIGH /v1/portcheck (both audits)**: a master emitting outbound UDP on request
  = reflection/amplification surface its provider can flag; per-IP RL can't bound
  AGGREGATE UDP across a distributed source set; "can't aim at a 3rd party" false
  under CGNAT. DECISION (RULE 1, don't ship a risky half-feature): REMOVED the
  standalone endpoint this round (it had no UI wired). Safe design noted for when
  the DIRECT port-check UI lands: FOLD the probe into the RL_CREATE-capped, real-
  session-tied /v1/host direct announce + a nonce the host echoes on the socket
  it just bound -- no standalone reflector.
- **R1 (master)**: silent direct->p2p downgrade on bad port -> FIXED (reject 400
  + reflect conn).
- **CLIENT F1/F2 (loading-cover strand on shutdown race in JoinLobby)**: FIXED
  (Fail() on every exit, not bare return). **F3 (host-boot cover strand on
  HostWithSave throw)**: FIXED (catch -> AbortHost + join_progress::Reset).
  **F4 (RefreshAsync inFlight_ latched true on a parse bad_alloc -> browser
  frozen at Refreshing forever)**: FIXED (try/catch, always clear inFlight_).
- CLIENT clean: announce-fail direct path coherent; SetListed thread-safe
  (announcer mutex); g_listedState atomic; self-lobby guard correct for direct.
- DEFERRED (LOW): F6 (ParseHostPort bare-IPv6 misparse -- master returns dotted
  decimal, harmless; would Fail->reconnect anyway); F7 (server_browser per-frame
  CopyRows -- main-menu-only waste; gen-guard is a trivial later win).
OPS: VPS master needs the new coop_master_server.py deployed (USER step -- the
  master is RULE-3 infra; the resolve_client_ip C-1 fix matters most). DLL +
  LAN smoke is the mod side.
