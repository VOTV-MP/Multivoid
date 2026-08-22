# IsLive zero-AV discipline: CachedObjRef + the 78-site conversion (DESIGN, 2026-08-22)

> **Design of record** for the "IsLive/VEH exit crash" WP-2 queue item. Converged via a 10-round `/qf`
> pass ("that holds" at round 10 of 15); the full Q&A transcript lives in the session scratchpad
> (`qf_thread.md`) — this doc is its durable distillation.
> STATUS: **D1 BUILT 2026-08-22 eve** — all 78 census sites converted across 8 staged commits
> (`f675de11` substrate → `d7f2904f` prime suspects [differential bracket PASS] → `3e38c62e`
> session/player [LAN smoke PASS] → `42edbb51` Element::LiveActor → `ecb7fb57`+`7cf2d009` props →
> `cb97d64e` ue_wrap+reflection → final dev/daynightcycle commit). Evidence: the deterministic
> decommit drill PASSED twice (pre- and post-conversion bytes: legacy exactly 1 absorbed AV with
> caller attribution, CachedObjRef 0 AVs), `tools/reflection/islive_gate.ps1` CI-mode **PASS** (0
> bare-IsLive-on-static), full LAN smoke on final bytes (see §4 addendum), ~354 bare IsLive calls
> remain — all fresh-same-task contract by census. NOT hands-on (run B pending the user).
> Related: `docs/UE4SS_ARC.md` §4 (the finding's origin), `docs/LESSONS.md` (the VEH-preempts-SEH row +
> the islive-recycled-slot row), `memory/lesson_veh_crash_reporter_preempts_seh_guard.md`.

## 1. The user's ask and the measured symptom

User exited a save to the main menu in the REAL modded env (r2modman/shimloader + experimental UE4SS +
CrashContext + Fusion/FusionFix/VoidFax/DebugMod/PBMovement) and got what looked like a crash — a
**popup** (user-confirmed 2026-08-22) + two `CrashContext_20260822_1545*.log` reports, both naming
`main.dll+0x11CC78` = `ue_wrap::reflection::IsLive`. ASK: exiting to the menu in the common modded stack
must not look like a crash.

Measured truth:
- **It was not a crash.** CrashContext has no `TerminateProcess`/`ExitProcess`/`MiniDumpWriteDump`/
  `SetUnhandledExceptionFilter`/`__fastfail` imports (byte-scan of its 46 KB DLL) — it is a VEH
  (`AddVectoredExceptionHandler` + `MessageBoxW`) that reports first-chance exceptions and passes them
  on. Our SEH then absorbed the fault as designed. Process survived: a second report 9 s later, and no
  UE crash dump in that window (newest = 13:13).
- **VEH runs before frame-based SEH** — any VEH crash reporter turns our "absorbed by contract" probe
  fault into a user-visible crash report. Reproduces only when the freed page is DECOMMITTED
  (freed-but-mapped reads succeed and `IsLiveByIndex` rejects cleanly) → nondeterministic; two DEV
  menutravel runs were silently clean while the real env faulted twice.

## 2. The root: a ratified discipline being violated at 78 sites

The project already ratified the fix's shape (OPUS_48_DISCIPLINE.md:59): **"Cached actor pointers across
ticks: IsLiveByIndex with the CAPTURED index, never bare IsLive"** — and LESSONS' islive-recycled-slot
row warns its own 2026-07-15 sweep slipped sites. A fresh two-agent census (2026-08-22, full tree minus
`harness/autotest`) measured:

- **435 `IsLive(` call sites** total; **357 fresh** (same-task pointers — legitimate bare-IsLive use);
- **78 VIOLATORS** — bare `IsLive` on pointers cached across ticks (53 in `coop/` [40 shipping + 13
  dev] + 25 in `ue_wrap/`+`ui/`); **≥132 prior-art sites** already use captured-index `IsLiveByIndex`.
- **Prime suspect for the user's symptom:** `ui/multiplayer_menu.cpp` `g_button`/`g_versionText` —
  widgets of the boot-time menu instance, freed for the entire play session, probed per menu tick
  exactly on RETURN to the menu (`:90/:122/:212/:225/:248`). Runner-up: `coop/input/input_owner.cpp`
  `g_localPawn` (per-WndProc-message probe, `:155`) + `g_lastOwner` (`:336`).
- The full violator tables (file:line, cache var, teardown-reachability, thread, idx-nearby) are in
  **Appendix A** — they are the conversion work-list.
- SEH census (all 12 `__try` sites classified): **IsLive is the ONLY normal-operation fault contract we
  own**; every other SEH frame is a bug firewall (ufunction_hook:58, pe_detour ×4, save_block:102,
  begin_equipment:24, imgui_overlay ×2) — a crash reporter reporting those is correct behavior.

**Acceptance criterion (adopted round 1): zero first-chance AVs from our probes in normal operation.**
Options rejected as RULE-1 crutches enabling continued violation: (a) syscall probe (VirtualQuery/
NtReadVirtualMemory — TOCTOU race + per-call cost), (b) our own first-position VEH (AV still raised
first-chance; ordering arms race), (d) accept-and-attribute (leaves the symptom).

## 3. D1 — the fix: `CachedObjRef` (ue_wrap/core) + 78 conversions

A small type carries the invariant so a cached use cannot silently regress:

- `CachedObjRef { void* ptr; int32 idx; int32 serial; }`
  - `Set(p)` — captures `InternalIndexOf(p)` + the FUObjectItem SerialNumber. **Contract: `p` must be
    fresh-or-just-ByIndex-validated in the SAME game-thread task** (Set derefs to read the index).
  - `Alive()` — `IsLiveByIndex(ptr, idx)` + serial rule; **array-slot reads only, never obj memory** →
    zero AV, valid off-thread (chunk table is never freed; torn reads → benign false).
  - **Serial rule (FWeakObjectPtr semantics, corrected round 6):** if captured serial != 0, current
    MUST equal it (mismatch INCLUDING current==0 → dead). Safe under either engine free-semantics.
    UE assigns serials LAZILY (0 until a weak ref exists) → captured==0 contributes nothing (see §6 ABA).
- All 78 violator sites convert to it. `IsLive(void*)` KEEPS its fresh-same-task contract (357 sites,
  documented in its header comment); its SEH guard becomes a **dead-man's brake** — expected never to
  fire; the caller-attribution WARN (shipped 2026-08-22, `_ReturnAddress` + module+offset, first 16
  unconditional) is the tripwire that names any residual site.
- **Staged landing** (s21–s28 cut discipline): per-subsystem commits + smoke each; the mechanical gate
  cross-check runs FIRST; the differential menutravel run brackets the multiplayer_menu/input_owner
  commit. Mega-diff rejected.
- **Fill-site review artifact:** the conversion produces a per-site table (file:line, cache var, fill
  origin classification, idx source, converting commit) kept as Appendix B; commits cite their rows.

## 4. Verification (two-sided + deterministic drill)

1. **Equality-on-live:** smoke digest — on live objects converted and legacy answers are identical.
   Honest coverage: proven only for executed sites; unreached dev paths rely on the mechanical sameness
   + the tripwire.
2. **Deterministic zero-AV drill** (dev-gated, DEV smoke lane only): VirtualAlloc a page, plant a fake
   InternalIndex, then (i) legacy bare IsLive on the DECOMMITTED page → exactly 1 first-chance AV,
   counted by the drill's own env-gated diagnostic VEH (registered inside the drill run only,
   RemoveVectoredExceptionHandler on exit — NEVER in normal boots or the modded profile); (ii)
   `CachedObjRef::Alive()` on the same input → false, 0 AVs. Engine-independent, deterministic.
   The drill proves zero-AV + slot-mismatch rejection; it structurally CANNOT exercise the ABA impostor
   (§6) and does not claim to.
3. **Differential menutravel:** `mp.py menutravel` with `VOTVCOOP_MENUTRAVEL_NO_BYPASS=1` pre/post the
   multiplayer_menu commit — real freed widgets, re-injection must behave identically.
4. **User runs, TWO, explicitly sequenced:**
   - **Run A (PRE-D1, profile build `F71621E0`):** one exit-to-menu in the modded env → the attribution
     WARN names the 15:45 caller. **OUTCOME 2026-08-22: attempted, NO repro** — no popup, no new
     CrashContext report, no WARN in the fresh log = zero probe faults that run (freed pages stayed
     mapped; the decommit nondeterminism now measured in the real env too, n=2 clean vs n=1 faulting).
     **Run A downgraded from precondition to OPPORTUNISTIC**: the tripwire persists in the D1 build
     (bare IsLive keeps the brake + attribution WARN), so caller attribution is never lost — any future
     natural fault still names its site. The 15:45 caller stays formally unnamed; the prime suspect
     (multiplayer_menu) stands on structural evidence, and D1's justification never depended on the
     name (round 4 A3). D1 may deploy to the profile once built + verified.
   - **Run B (POST-D1):** same exit → acceptance = **no CrashContext report whose faulting module
     resolves into main.dll** (other mods' AVs are not ours to claim or fail on) + no popup. Given the
     nondeterminism, run B is necessary-not-sufficient hands-on evidence; the deterministic drill (§4.2)
     carries the zero-AV proof.

## 5. D3 — the ratchet

A tripwire gate script (nick_gate/peerconn_gate family): flags file-scope/static raw pointers later fed
to `IsLive`, and the ad-hoc `{ptr, idx}` pair smell. **Honest billing (round 7):** it mechanically
re-derives the STATIC subset only; member-field caches (ActiveDrive rows, element structs) are invisible
to it — those are covered by the census (Appendix A), sampled review, and the runtime tripwire. The
TYPE is the invariant; the script is a tripwire, not a proof.

## 6. Residuals — filed, not hand-waved

- **ABA/impostor identity defect (PRE-EXISTING, tree-wide):** `IsLiveByIndex` compares slot pointer +
  kill-flags, never SerialNumber → a same-address successor landing the SAME recycled slot passes. This
  exists TODAY in all ≥132 prior-art sites; D1 does not regress it and opportunistically hardens it
  (serial rule) where the engine happened to assign serials. Truly closing it needs the engine's
  `AllocateSerialNumber` (unreflected native; IDA cost; writes GC state) — **its own future thread**,
  not part of this arc. Consequence of the residual: a wrong-but-LIVE object passes (logic defect, no
  AV) — it cannot produce the crash-report symptom.
- **D2 — the purge-blind world gate (designed, DEFERRED, demotion CONDITIONAL):** `worldUp =
  Registry::Local() != nullptr` accepts the DYING pawn (kill-flags lag travel — measured: 311 dying-
  world piles passed IsLive filters AT the menu, 16:46:03), so the session chain runs ≤4 s against the
  dying world until the flee's 4 s poll fires. Post-D1 the window cannot fault (caches become
  ByIndex-validated) — residual harm = wasted work + possible WIRE sends about dying actors. Guards
  exist (reaper death-watch gated `!IsFleeing() && session.running()`, registry_reaper.cpp:158; destroy
  seam episode gates, prop_destroy_seam.cpp:116) but the 3-clock RACE (flee 4 s poll vs reaper 4 s scan
  vs GC purge) is UNMEASURED. **Probe item 1 (runs BEFORE this arc closes): two-peer DEV measurement —
  client native-quits with session live; census the HOST's inbound wire log for the window.** Zero
  leakage → D2 stays deferred; any leakage → D2 re-prioritizes.
  **PROBE RESULT (2026-08-22 18:17, RUN — `mp.py wirewindow`, DLL E69A528307B741CF): ZERO leakage →
  D2 STAYS DEFERRED.** Mechanism measured: the client joined, dwelled 25 s steady (baseline
  [-5s,0): 0 reliables, ~60/s PoseSnapshot only), then `transition(/Game/menu)` with the layer
  LIVE. The window [0,+10s] on the host: **0 reliables; poses continue ~2 s then stop**. The window
  is closed not by the 4 s flee poll but by an EXISTING menu-edge detector at **+1 s** (client log
  18:17:52 `net: gameplay->MENU while a session is live (VOTV quit-to-menu?) -- ending the session`),
  which Stop()s the session → host sees `peer slot 1 closed (reason='session stop')` + the full
  OnDisconnect fanout, all clean. The destroy-seam episode gates held (world-teardown mass
  K2_DestroyActor produced zero PropDestroy on the wire). So the purge-blind window's wire-leak leg
  is ~1 s and pose-only on this path; the residual harm shrinks to wasted local work. Caveats: n=1,
  IDLE client (no held props / mid-action state at exit); the census instrument is permanent
  (`coop/dev/wire_census` + `mp.py wirewindow`) so an adversarial variant (exit while holding /
  mid-action) is a one-command rerun if D2 is ever re-opened. Bonus: zero `IsLive caught` WARNs on
  this live-layer exit too (third consecutive clean run — the decommit nondeterminism holds).
  D2's shape when built: ONE owner
  (`world_watch`, ue_wrap/engine) publishing current-world identity + tearing-down per tick; flee tell
  subsumed (RULE 2); reaper keeps its cadence reading shared truth. Seam candidates (probe decides):
  `UWorld::bIsTearingDown` (unreflected, +1 IDA offset on the version surface), current-world swap
  timing, transition-dispatch PE-visibility (the init-is-BP-internal trap risk). NOTE: engine.cpp's
  `g_worldContext` is the GAMEINSTANCE (immortal) — NOT a current-world read; do not reuse it for this.
- **Ad-hoc pair migration scope (USER DECISION PENDING):** the ≥132 prior-art `{ptr, idx}` hand-rolled
  pairs are the same concept as CachedObjRef (RULE 2: one concept, one implementation). Default intent:
  migrate them in the same arc (mechanical, ~90 files); droppable if the user prefers type+violators
  only.
- **Two impl-gate measurements (queued, design robust to either outcome):** IDA confirm
  `CollectGarbage`/purge is GT-only (backs the fresh-in-task safety inference; if wrong, the SEH brake +
  tripwire still catch it) and `FreeUObjectIndex` serial-zeroing semantics (documents which serial-rule
  leg fires).
- **User-facts (both answered 2026-08-22):** popup CONFIRMED (user: "I will reproduce the pop up");
  session-live CONFIRMED — the user was HOSTING at 15:45, so the ≤4 s running-session purge-blind
  window (§6 D2) applies to that session's exit, and run A's wire-window relevance is real, not
  hypothetical. Run A itself still pending.

## Appendix A — the 78-site violator census (2026-08-22, two agents, verbatim work-list)

### coop/ (53: 40 shipping + 13 dev) — format: file:line cacheVar teardown-reachable? thread idx-nearby?

Shipping — actor/instance caches:
```
session/net_pump.cpp:661   g_netLocal                      YES (every 125Hz pump tick)   GT   no-idx
session/net_pump.cpp:719   g_netLocalController            YES (same pump tick)          GT   no-idx
player/players_registry.cpp:113  localCached_ (Registry)    YES (per-frame, incl. menu)   GT   no-idx
player/players_registry.cpp:67   sMpClass (static UClass)   YES (cold rescan)             GT   no-idx
player/local_streams.cpp:218     g_lastHeldProp             gameplay-tick                 GT   YES (g_lastHeldPropIdx; :518/:585 already use it)
player/hand_item.cpp:522   RemotePlayer::actor_ via pup->GetActor() (no valid())  gameplay-tick  GT  YES (rp->valid())
creatures/kerfur_command.cpp:138  RemotePlayer/localCached_ via ResolveOwnerBody()  gameplay-tick  GT  YES
creatures/wisp_grab_hold.cpp:50   el->GetActor() (ResolveWispActor)   gameplay   GT   YES (el->GetInternalIdx())
creatures/wisp_tear_mirror.cpp:65 el->GetActor() (caller :90)         gameplay   GT   YES
creatures/wisp_attack_sync.cpp:205  element pending-destroy resolve    gameplay   GT   YES
creatures/wisp_attack_sync.cpp:309  npc->GetActor() (Snapshot walk)    gameplay   GT   YES
creatures/npc_mirror.cpp:442  drained->GetActor() (OnDestroy)   gameplay   GT   YES (twin world_actor_mirror.cpp:212 already fixed)
creatures/npc_mirror.cpp:446  same                              gameplay   GT   YES
save/save_transfer.cpp:718   el->GetActor() (pileM walk)   join-window GT task   GT   YES
save/save_transfer.cpp:740   el->GetActor() (keyedM walk)  same                  GT   YES
interactables/window_sync.cpp:243  g_byKey[].actor (drift probe)  gameplay-tick host  GT  YES (entry .idx; 4 sibling sites use it)
props/remote_prop.cpp:300   g_drives[slot].actor    per-PropPose receive   GT   YES (ActiveDrive::actorIdx)
props/remote_prop.cpp:140   drive.actor (DriveTogglePhysics)   gameplay release   GT   YES
props/remote_prop.cpp:150   drive.actor (StickHoldsPhysicsOff) gameplay release   GT   YES
props/trash_clump_pose_stream.cpp:74  g_carryDrives[].actor   gameplay-tick   GT   YES [AddToRoot-ed targets — verify exemption]
props/trash_proxy.cpp:166,201,222,246,265,280,296  g_proxies[].actor (7 sites)  gameplay  GT  no-idx [AddToRoot-ed — verify exemption]
props/prop_destroy_seam.cpp:213  lambda-captured actor in deferred DestroyLocalProp   gameplay (LATER GT task)   GT   no-idx
input/input_owner.cpp:155  g_localPawn (KEYSTROKE path)   YES (per-WndProc-message, incl. menu)   GT   no-idx
input/input_owner.cpp:336  g_lastOwner (widget)           YES (1 Hz scan incl. menu)              GT   no-idx
save/save_button_disable.cpp:90  g_menuInstance             YES (per ESC press)                    GT   no-idx
```
Shipping — static UClass/CDO/asset caches (self-healing but bare):
```
props/prop_fresh_spawn.cpp:42     g_gsCdo          gameplay   GT   no-idx
props/remote_prop_physics.cpp:84  g_primCompCls    gameplay   GT   no-idx
props/remote_prop_destroy.cpp:50  g_actorCls       gameplay   GT   no-idx
props/prop_destroy_seam.cpp:202   sActorCls        gameplay   GT   no-idx
props/trash_proxy.cpp:92          sDirtball        gameplay   GT   no-idx
props/trash_proxy.cpp:104         sCube            gameplay   GT   no-idx
```
Shipping, ini-gated: save_identity_bind.cpp:324/:656/:669 (el->GetActor(), idx nearby).
dev/: flashlight_setup.cpp:39 g_cache.mainGameInstance; freecam.cpp:87/:183(x2)/:185/:191/:219(x2)
(g_player/g_pc/g_camActor); pos_hud.cpp:53/:89/:127 (g_root); drone_probe.cpp:79 g_gmCache;
spawn_npc.cpp:135 pp->GetActor().
Adjacent (header): include/coop/props/active_drive.h:133,151 bare-IsLive d.actor (has actorIdx).

### ue_wrap/ + ui/ (25) — same format

```
ue_wrap/world/economy.cpp:18            g_gm             Y (host tick)      GT  no
ue_wrap/world/skysphere.cpp:70          g_skyCache       Y (weather tick)   GT  no
ue_wrap/world/directionalwind.cpp:22    g_windCache      Y (weather tick)   GT  no
ue_wrap/actors/vitals.cpp:43            g_cache.gameInstance  Y (vitals tick)  GT  no
ue_wrap/engine/engine_pawn.cpp:74       g_camMgr         Y (per-frame)      GT  no
ue_wrap/engine/engine_mainplayer.cpp:53  g_phcClsCache   N (dispatch)       GT  no
ue_wrap/engine/engine_mainplayer.cpp:124 g_hurtMat       N (edge)           GT  no
ue_wrap/engine/engine_mainplayer.cpp:192,196 saved[i].component/.original (held ~0.5s)  N  GT  no
ue_wrap/actors/prop.cpp:36              g_propBaseCls    Y (spawn-catch hot)  GT  no
ue_wrap/actors/prop.cpp:542             g_pvr.cls        Y (pose-tick)        GT  no
ue_wrap/actors/prop.cpp:730             g_sce.cls        N (dispatch)         GT  no
ue_wrap/engine/engine_playerragdoll.cpp:71  g_ragdollClass   N   GT  no
ue_wrap/engine/engine_playerragdoll.cpp:154 sPrimCompCls     N   GT  no
ue_wrap/desk/device_screen.cpp:288      g_saved.player (whole interface session)  N  GT  no
ue_wrap/actors/puppet.cpp:162           g_meshComp[actor] (map value)  Y (per puppet frame; actor-outlives-comp window documented)  GT  no
ue_wrap/devices/door.cpp:342            g_verify key door (<=1.5s)  Y (tick while pending)  GT  no (sibling door_box HAS idx)
ue_wrap/world/order_economy.cpp:47      g_gm             Y (order tick)     GT  no
ue_wrap/actors/inventory.cpp:100        g_gm             N (edges)          GT  no
ue_wrap/core/reflection.cpp:380         g_classCache entry  Y (every FindClass, incl. menu)  ANY THREAD  no
ui/multiplayer_menu.cpp:90,122,212,225,248  g_button/g_versionText  Y (MENU TICK = the symptom context)  GT  no
```
Notes: multiplayer_menu = prime suspect (freed whole play session, probed on menu return; fill sites
DoInject :93-96, UpdateVersionLabel :125-131). reflection.cpp:380 runs on ANY thread. Three g_gm copies
(economy/order_economy/inventory) unmigrated while five siblings (sleep:103, saved_signals:50, dish:122,
meadow_store:39, email:71) already use ByIndex. Prior-art examples: door_box.cpp:233, desk_audio.cpp:161
(+8), engine.cpp:62 (g_worldContext/idx — the GameInstance).

## Appendix B — fill-site conversion table

Per site: cache var, fill origin (what makes Set()'s fresh-same-task contract hold), converting
commit. Line numbers are the PRE-conversion census rows (Appendix A).

| file (census rows) | cache var | fill origin | commit |
|---|---|---|---|
| ui/multiplayer_menu.cpp :90/:212/:225/:248 | g_button | out-param of E::InjectCanvasButton, same GT task | d7f2904f |
| ui/multiplayer_menu.cpp :122 (+:151/:156 steady) | g_versionText | out-param of E::InjectTextRowAbove, same GT task | d7f2904f |
| coop/input/input_owner.cpp :155 (alias mp) | g_localPawn | Registry::Local() (validates internally), same GT task | d7f2904f |
| coop/input/input_owner.cpp :336 | g_lastOwner | ObjectAt walk + fresh IsLive in the same scan | d7f2904f |
| session/net_pump.cpp :661/:719 | g_netLocal / g_netLocalController | Registry::Local() this tick / GetController(fresh) | 3e38c62e |
| player/players_registry.cpp :113/:67 | localCached_ (member) / sMpClass | RescanLocal walk / class walk hit | 3e38c62e |
| player/local_streams.cpp :218 | g_lastHeldProp (ad-hoc {ptr,idx} pair RETIRED into the type) | new-held edge, IsLive-guarded branch | 3e38c62e |
| player/hand_item.cpp :522 | RemotePlayer actor via valid() | RemotePlayer's own captured idx | 3e38c62e |
| creatures/kerfur_command.cpp :138 | ResolveOwnerBody root-validated | Local()/valid() inside the resolver | 3e38c62e |
| wisp ×4, npc_mirror :442/:446, save_transfer :718/:740, save_identity_bind :324/:656/:669, dev spawn_npc :135 | element actors | Element::LiveActor() (the element's own captured idx) | 42edbb51 |
| props/remote_prop.cpp :140/:150/:300 + active_drive.h :133/:151 | drive actors | ActiveDrive::LiveActor() (actorIdx) | ecb7fb57 |
| props/trash_proxy.cpp ×7 + :92/:104 | ProxyEntry.actor (rooted-exemption claim REJECTED: teardown kills rooted actors) + mesh caches | Set at SpawnActor / FindObject | ecb7fb57 |
| props/prop_destroy_seam.cpp :213/:202 | lambda-captured ref (by value) + sActorCls | Set at the call site (callers pass fresh) | ecb7fb57 |
| props/prop_fresh_spawn :42, remote_prop_physics :84, remote_prop_destroy :50 | class/CDO caches | FindClass/FindClassDefaultObject | ecb7fb57 |
| interactables/window_sync.cpp :243 | entry {actor,idx} copied under the lock | entry's own idx (sibling shape) | ecb7fb57 |
| save/save_button_disable.cpp :90 | g_menuInstance | FindObjectByClass | ecb7fb57 |
| props/trash_clump_pose_stream.cpp :74 | carry ActiveDrive | LiveActor() | 7cf2d009 |
| ue_wrap economy :18 / order_economy :47 / inventory :100 (g_gm ×3), skysphere :70, directionalwind :22, vitals :43, engine_pawn :74 | singletons | FindObjectByClass | cb97d64e |
| ue_wrap engine_mainplayer :53/:124/:192-196 (SavedMaterial in types.h), prop.cpp :36/:542/:730, engine_playerragdoll :71/:154 | class/material/component caches | resolve walks / GetMaterial fresh | cb97d64e |
| ue_wrap device_screen :288, puppet.cpp :162 (map values), door.cpp :342 (VerifyEntry.ref; key compare-only) | session/frame caches | enter seam / spawn path / apply seam | cb97d64e |
| ue_wrap/core/reflection.cpp :380 | CachedClass.cls (ANY THREAD) | PrimeClassWalk (caller's fresh walk) | cb97d64e |
| dev flashlight :39, freecam ×7, pos_hud ×3, drone_probe :79 | probe caches | resolver walks / spawn out-params | final commit |
| ue_wrap daynightcycle (ad-hoc pair retired; not a census violator — gate CI residual) | g_cycleCache | FindObjectByClass | final commit |

Differential bracket for this commit (design §4.3): PRE `CEAA0E93` vs POST `52FFF0F7`, solo
no-bypass menutravel each — all 7 markers identical (LAYER LIVE dispatch → version label + button
INJECTED on the RETURNED menu → menu-edge session stop → bypass RESUMED → SHOT READY → DONE →
process alive), WARN class sets byte-identical after stripping run-identity noise (ASLR digits,
fresh-save re-key suffixes), 0 IsLive WARNs both sides. Logs: scratchpad `diff_pre3.log` /
`diff_post.log`.
