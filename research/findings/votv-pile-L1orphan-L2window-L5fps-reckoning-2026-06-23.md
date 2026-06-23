# Pile L1 (orphan) + L2 (interaction window) + L5 (FPS) reckoning — 2026-06-23

> **CORRECTION 2026-06-23 (later, decisive hands-on) — the L1 "orphan / absence-removal at the JOIN"
> direction in this doc is SUPERSEDED + CANCELLED.** A controlled hands-on proved the real L1 bug is a
> JOIN-WINDOW two-channel DUP, not a join-time orphan: pre-connect host moves go INTO the fresh scratch
> save → no pre-connect divergence exists, which is why every census read 0 (correct, not a bug). The
> real dup is a host pile MOVED during the join-load window — the scratch-save native (old pos) + the
> broadcast-convert proxy (new pos) diverge >1cm and the 1cm position-dedup can't match them. Canonical:
> **`votv-pile-dup-join-window-two-channel-RE-2026-06-23.md`**. The L1 sections below (adopt-dead / the
> 3 gates / absence-removal-after-quiescence) are HISTORICAL — adopt is still dead, but the FIX target
> moved off the join entirely. **L2 + L5 sections below are still current.**

Point-in-time DESIGN + durable RE. A real two-peer HANDS-ON of the v85 chipPile client-grab chain
(dll `11135488`) surfaced 5 layers. L3 (carry jitter) + L4 (wild throw) were FIXED + `[V hands-on +
V harness]` (commit `92a76f27`, see `docs/piles/08`). This finding records the durable RE for the three
that remain (L1, L2 open; L5 parked). HEAD at writing: `54a3a332`, push held.

## L5 — FPS ~3-4s stutter: PARKED, walks are NOT the root (durable lesson)

Four passes (re-seed gate -> atv/grime gate -> all-6 gate + profiler -> incremental tail-scan of all 7
`*_sync` walks). The `ScopedWalkTimer` profiler settled it: the periodic index-rebuild walks WERE 5-20ms
each (real cost, worth removing), and the incremental tail-scan (`coop::util::IncrementalObjectScan`,
commit `54a3a332`) KILLED them — `[WALK-TIME]` dropped 69 -> 12, the walks now fire only their rare ~20-60s
safeties. **But the hands-on shows the ~3-4s hitch PERSISTS** -> the walks were never its root; the period
was a coincidence matched across 4 passes. Barely noticeable on the native FPS counter (cosmetic). A
fixed-period both-peers hitch is the textbook UE-GC signature (the game logs no GC by default -> 0 hits,
unconfirmed without a `CollectGarbage` hook). PARKED. The `reseed:KnownKeyedProps` (`prop_element_tracker`,
~20ms/20s) is the one periodic full-walk left un-converted, also parked. Lesson:
`[[lesson-periodic-hitch-not-the-walk-by-period-coincidence]]`.

## L1 — orphan / client->host level-pile: adopt is DEAD (3 gates), fix designed not built

Symptom: a CLIENT grabbing an ORIGINAL level-pile is not translated to the host. The "adopt the client's
own native pile onto the host eid" idea is DEAD on three independent gates (agent-verified, code+bytecode):

1. **Drain-rot (Q1):** the native `actorChipPile_C` is provably static (no `SetLifeSpan`/`ReceiveTick`/
   self-morph, `actorChipPile.json`) but the bind points at a GC-managed actor the join drain frees; a
   re-adopt barrier on `HasLoadTailQuiesced` is heuristic (45s deadline can fire mid-load) = probabilistic,
   not provable. The proxy holds the actor static by construction (`AddToRoot` + no-BP).
2. **No suppress-native (Q2):** a client grabbing a real `actorChipPile_C` runs `playerGrabbed` via
   `EX_LocalVirtualFunction` — BELOW our only hook; no suppress-native is architecturally possible
   (`COOP_DISPATCH_VISIBILITY.md:108-116`) -> two-sided sim drift (the exact bug the proxy model killed).
3. **The "12 of 871" was a LOG-THROTTLE artifact (Q3, decisive):** the destroy log throttles to first-8 +
   every-200th (`remote_prop_spawn.cpp:409`) = 12 lines, but the destroy actually fired ~801+ (the
   "native(s) left in index" counter ran 871 -> 70). Deterministic positions HOLD (92% match at 1cm). 30cm
   adopt is UNSAFE (cluster piles are 5-50cm apart -> binds a neighbour).

**Real L1 bug:** ~70 host state-drift orphan natives (the host moved/collected piles since the save). The
native-twin destroy is gated on `g_claimTrackingActive` (the snapshot burst), NOT on `HasLoadTailQuiesced()`
-> natives the drain frees+recreates AFTER a proxy's destroy-pass linger. And those ~70 are the only
level-piles that still show a native "press E" window (real `actorChipPile_C`), so a human aims at exactly
the orphans that grab locally + don't translate — L1 AND L2's missing-window, one root.

**FIX (designed, NOT built):** re-run the native-twin reconcile gated on `HasLoadTailQuiesced()`. The
existing divergence sweep ALREADY doom-removes un-mirrored chipPiles (`remote_prop_spawn.cpp:1123`, FACT
verified) — so the fix may be mostly "run it after quiescence" + an absence-removal pass (destroy a live
un-mirrored native with no host pile in tolerance, gated on the existing >50% world-wipe valve). The
`[PILE-DELTA]` probe is BUILT (the `matchCount==0` branch, env `VOTVCOOP_PILE_DELTA_PROBE`, read-only) but a
clean smoke shows ZERO orphans (no host-drift = no orphans) — the histogram NEEDS a host-drift scenario
(host moves ~5 piles pre-join) + insertion #2 (post-quiescence orphan census) to populate it. The user's 3
absence-removal safety asks to confirm at build: (1) quiescence = host expression COMPLETE (absence is real,
not a prop still in flight); (2) the >50% valve aborts an anomalous census; (3) absence-removal ONLY for the
>30cm true-drift class, near-miss (1-5cm) -> a tight-position re-destroy — split by the histogram tail.

### UPDATE 2026-06-23 (4-test hands-on) -- the host->client carry "FREEZE" is an L1 orphan symptom, NOT a carry regression
A 4-run hands-on isolated it with a clean CONTROL: when NO native orphan piles exist, host<->client
interaction (piles AND clumps) is SMOOTH on both sides. The carry "freeze" (the clump updates only on
the host's E-press, seen from the client) appears ONLY when native orphans exist AND only in the
host-grab direction. Read (user hypothesis): the client's native orphan coexists with the proxy at ~the
same spot, and host-grab makes the LOCAL (network-dead) native compete with the proxy carry-stream ->
the jerky "update only on E". So absence-removal (Phase 2) would fix TWO symptoms at once -- the visible
DUP and the carry-freeze -- which RAISES L1's priority. STATUS: strong correlation (the no-orphan
control is decisive), but the MECHANISM is a HYPOTHESIS pending (a) a code-trace of whether a co-located
native can throttle / mis-resolve the TrashCarryPose apply (in flight 2026-06-23), and (b) the
definitive proof = re-run the host-grab carry WITH a drift/orphan scenario AFTER absence-removal and
confirm the freeze is gone. NOT confirmed from the correlation alone (verify-don't-guess).

## L2 — proxy outside the native interaction system: RE done, not built

The client mirror is a bare `AStaticMeshActor` proxy, invisible to the native interaction system. Hands-on
confirmed 3 symptoms of one root: (a) no native "press E" WINDOW on aim, (b) the native ERHHH error sound
(E into the void), (c) the carried clump renders ABOVE the crosshair. Collision won't fix it (the
`int_player_C` filter on `lookAtActor`); adopt is dead. Direction = MOD PARITY (build the mod side up).
RE (durable, `research/bp_reflection/mainPlayer.json`, `ui_UI.json`):

- **Window — mod-DRIVE the NATIVE prompt.** `openHovertext` + `setLeftSideText` are methods on the
  `ui_UI_C` HUD widget held by the player's `gamemode` member (typed `ui_UI_C`, not the player); they do NOT
  touch `int_player` (that gate is upstream in `LookAtFunction`). On a cone-tick, read `localPlayer->gamemode`
  and call them -> a native-LOOKING "Grab" prompt with no interface. Edge cases (confirmed with the user):
  write ONLY when the native `lookAtActor` is empty (single-writer, no flicker vs the native); CLOSE on
  cone-invalid (the `openHovertext` hide param) + on a successful grab (gate on `cone-valid AND !carrying`).
- **ERHHH — narrowest seam.** The buzz is `UGameplayStatics::PlaySound2D(use_deny)` from the invalid-target
  branch of `useAction` — a VISIBLE `EX_FinalFunction`. `PlaySound2D` fires 33x, so gate the suppression on
  BOTH `Sound == use_deny` AND **our trash system ACTED on this E-press** (sent a GrabIntent/ThrowIntent) —
  strictly tighter than "cone valid"; a legitimate deny (a different cause) stays audible.
- **Clump-above-cursor — head bone vs eye.** `puppet_carry_drive` anchors at `GetHeadPosition()` = the head
  BONE +33cm (a nameplate lift, `remote_player.cpp:875`); native holds at the camera EYE
  (`Camera.location + fwd*grabLen`). Fix = anchor at the eye (drop the +33).

### UPDATE 2026-06-23 (4-test hands-on) -- L2 SYMPTOM 4: a proxy-clump cannot be thrown on LMB
Hands-on: standard (native) held props throw on LMB perfectly (whoosh, mirrored). A client-grabbed
proxy-CLUMP (pile -> cone-grab -> clump) CANNOT be thrown on LMB. Same L2 root: the proxy is invisible
to the native interaction system, so the native LMB-throw (which acts on the native held object) never
sees the proxy-clump, and the mod release is wired to the E toggle (InpActEvt_use -> ThrowIntent), NOT
to the native LMB-throw input. FIX (L2-parity, like the window / ERHHH): RE the native throw-input
UFunction (the LMB / "throw held" edge) and route it to SendThrowIntent for a cone-held proxy-clump,
mirroring how native props throw on LMB. DESIGN Q at RE: throw-on-LMB should mirror native; the E-throw
may then be redundant (RULE 2 -- one input) or kept as a convenience. GOOD BASELINE confirmed the SAME
run: native prop mirror/sync + LMB whoosh-throw work perfectly -- the bugs are specifically
orphan-induced (L1) or proxy-clump-specific (L2), NOT the base sync.

## NEXT (user's re-set priority): L1 (functional, fixes DUP + carry-freeze) -> L2 (window/ERHHH/eye-anchor + LMB-throw) -> puppet-init + L5-GC backlog.
