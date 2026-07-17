# v116 — the catch-attribution retire + the laptop_C lane (DESIGN OF RECORD, 2026-07-17 nite)

Status: AS-BUILT `613f2ac4` (+ ue_wrap split `9d24ac0c`), proto 116, smoke x3 PASS, NOT hands-on
(runbook take 4). /qf: 9 rounds total, two independent "that holds" (round 6 = the catch design,
round 9 = the laptop design). Fact base: the preserved 17:00-17:09 live logs
(scratchpad/live_logs_1704, copied BEFORE rotation) + `votv-laptop-pc-RE-2026-07-17.md`.

## 1. The measured root (take-3 reports R2-R5 = ONE eaten edge)

Timeline (live logs, both peers):
- 17:03:11 client claims the desk; 17:03:59-17:04:00 HOST DENIED E x4 (busy).
- 17:04:32 client stands up mid-ping #2 -> desk FSM-hold (v115b) holds the claim for slot 1.
- **17:04:46 ping #2 SUCCEEDS on the client machine** (desk_diag: coordSig None -> 'garb_0').
- **17:04:47 the FSM-hold releases** (coord_isPing fell — the success itself ends the hold).
- ZERO `signal_catch` wire lines in either log: the 1 Hz CLAIM-GATED catch detector
  (`RunDetectors`, gated on `LocalHolds("desk")`) lost the race, and the unconditional baseline
  roll-forward consumed the edge PERMANENTLY.

Cascade: host coordSig=None all session -> NO SIGNAL + partial polarity (R5) + no dish theater
(the host is the ONLY pose author since v113) -> mirrored dishes frozen (R3); the client's
`KillOwnPingSlews` lives INSIDE the never-fired detector -> its 24 dishes self-slewed to garb_0
(desk_diag moving 0->24 at 17:04:46) -> the window-visible dish divergence (R4); the fused
FSM display showed triangulation only to the pinger (R2; v115b residual R-a).

**The class**: any gate anchored on a claim/occupancy that the GATED EVENT ITSELF releases loses
the race BY CONSTRUCTION when the detector is slower than the release path.

## 2. FIX-A: retire claim-based catch attribution (qf rounds 1-6)

Post-v115b invariant, promoted to MEASURED by a tree-wide grep (round 1): the ONLY writers of
`coord_signalData` are `WriteCoordSignal/ClearCoordSignal` (ue_wrap), called solely from the
priming `ApplyReplay`/connect-seed — so an UNPRIMED identity change-edge on a peer can only be
that peer's own FSM success. Therefore (RULE 2):
- detector `LocalHolds` gate — RETIRED; `NoteIncomingSnapshot` pre-gate — RETIRED;
- host `holder==sender` validator — RETIRED, replaced by a host-side `IsRecent` dup guard
  (round 4; also protects an in-progress download from a dup's ResetDownloadMachine);
- `kind=2` connect STATE-SEED (round 2): applied exactly like kind=0 but NEVER announced to the
  feed (a joiner must not see a stale "caught" line); host drops incoming kind=2 (round 5);
- `ReadSlewFromMovingDish` fallback (round 5): moving dish preferred, else the FIRST live dish's
  `lookAt` — the catch chain writes lookAt ABSOLUTE to ALL 24 dishes at the catch, so a settled
  array still holds the fresh target within the <=1 s poll window; without it a near-aim catch
  shipped slewValid=0 and the host never armed (no theater -> no dishesStop -> no formDownload);
- disconnected-window edges are covered by the STATE seed (QueueConnectBroadcastForSlot reads
  live ground truth), not by edge repair (round 1, the connect-seed lesson shape);
- IsRecent TTL 5 s is safe: a same-identity re-catch needs a full ping cycle, measured >= 6 s
  (round 3);
- the v18 logical-origin stamp (SendReliableToSlot arg 5) already carries the true catcher to
  third clients (round 3 — no new payload field needed).

FEATURE (user request mid-session): every kind=0 catch lands ONE `peer_action_feed` line per peer
("You caught signal 'X'" at the catcher's detector; "<nick> caught signal 'X'" at the two
OnReliable sites). Product calls SURFACED to the user, decisions pending:
(a) observer mid-ping triangulation display (fused flag — needs a display-only mirror design);
(b) NO feed on kind=1 (this seam cannot distinguish the delete button from the download-complete
save @27793 / virus @78527 auto-clears — discrimination belongs to L6/OPEN-9).

## 3. FIX-D: the laptop_C lane (qf rounds 7-9 + the post-R9 simplification)

RE base `votv-laptop-pc-RE-2026-07-17.md`. Key round outcomes:
- R7-Q1 + POST-R9: the disc-destroy axis has ONE owner — the pre-existing v106 K2_DestroyActor
  Func-seam already crosses keyed destroys on BOTH roles (prop_destroy_seam.cpp), which DISSOLVED
  the planned BndEvt PRE/POST eid-capture entirely (and R7-Q2's stash race with it). Receivers
  NEVER destroy on LaptopState.
- R7-Q3/R8-Q3: disc CONTENT lives WITH the prop under HOST authority: DiscContent{eid, chunks};
  a client-eject's content correlates via the ADOPTION eid-binding on the client's own actor (no
  nonce, no PropDropIntent change); joiners get ground-truth-read rows at connect.
- R7-Q4: v1 SCOPE CUT — PC buffer + portable PC OUT (TRACKER OPEN-10; shared claim key 'laptop'
  = the per-device discrimination question).
- R8-Q4: actionOptionIndex(b8) empty-frame replay proven by beginplayTurnOn@815 (in-game
  precedent); param literally named `Action`; ParamFrame zeroes the rest.
- Audit fixes (post-"that holds", both verified by rebuild+smoke):
  - perf IMPORTANT: the fstring PIN doctrine holds only for FRESH buffers — repeated in-place
    mints on the same live laptop fields leaked engine buffers; now swap-and-EngineFree
    (`FreeFStringSlot/FreeFStringArraySlot`, laptop.cpp).
  - correctness IMPORTANT-1: applying insert SCALARS before the content chunks opened a window
    where a receiver-side eject spawned a content-less disc canonically; now the OCCUPIED apply
    is ATOMIC — op=1/3 park in `g_pendingSlot`, scalars+strings land in ONE WriteSlot at chunk
    assembly (10 s TTL scalar-only fallback, WARN).
- Flagged, deferred: event_dispatch_state.cpp at 791/800 LOC — extract `event_dispatch_signal.cpp`
  BEFORE the next ReliableKind case (L6).

## 4. FIX-C: the cursor (R1) — honest state

Root NOT proven. Removed measured noise: the deployed inis ran a closed-measurement diag battery
(kerfur_census=1 = a full ~281k GUObjectArray walk, 8-25 ms every 10 s, inside sync:npc_client;
+ rng_roll_census, weather_probe, vm_dispatch_log, pile_dup_probe, garbage_pickup_probe,
pile_delta_probe) — all OFF in all 4 installs; desk_diag KEPT (the proven discriminator); the
HOST gained perf_probe=1 (host fps was never measured; the mirror-cursor observer IS the host).
The npc_client ~30 ms/tick walk reproduced in the v116 smoke WITH census off — the mirror Tick
walk is the remaining named suspect (smoke-env-conditioned). Attribution set for take 4 in
TRACKER OPEN-1.

## 5. Verification state

Build clean x3; perf audit PASS (1 finding -> fixed); correctness audit 0 CRIT, 1 IMPORTANT ->
fixed; smoke PASS x3 (final on the shipped bytes); DLL `bcf0f58e4423cb66` x4 hash-verified
(sort -u = one line); 0 WARN/ERROR from the new lanes in the smoke logs; `laptop: resolved` with
live offsets IDENTICAL to the RE (0x418/0x450/0x4C8, no fallback WARN). PUSHED bfe01b28..9d24ac0c
after a clean leak audit (no never-commit files, no secrets/IPs in the 17-commit diff).
