# Grab/throw chain + join-window bind-vs-mutation — RE + as-built (2026-06-26)

Point-in-time record of the 2026-06-26 session. The save-identity bind (the stable-ID thread) was already
autonomous-verified; this session took it through hands-on and then closed the interaction + join-window
edges it exposed. Status tags: **VERIFIED** = hands-on or matching real log; **AS-BUILT** = committed,
compiles; **DESIGN** = on review, not built.

## 1. #3 grab-release E-drop — VERIFIED (commit `2db3df27`, PUSHED in `eb85ddfb`)
**Symptom (hands-on 12:02):** a client grab of a save-loaded NATIVE pile stuck in carry forever; the host
slot stayed latched so every later grab was DENIED.
**Root (pinned):** the client grab path morphs the bound native into a clump proxy (the CRITICAL-1 hand-off
in `remote_prop.cpp` OnConvert). The release path EXISTS (the E-press THROW toggle in `OnPileGrabPre`, keyed
on `ClientCarryEid()`), and `g_clientCarry` is armed by `NoteClientConvertObserved` — but only the `IsProxy`
convert branch called it; the **morph branch omitted it**. So the carry latch never armed → the toggle never
fired → stuck.
**Fix:** one line — `NoteClientConvertObserved(E, wantClump)` before `return proxy` in the morph branch
(`remote_prop.cpp` ~:1090), mirroring the IsProxy branch (~:1039).
**VERIFIED 12:36:** ~8 grab/E-drop cycles on two native piles, `carry CONFIRMED`, `ToPile` re-pile drift
0.00cm, zero DENIED. The autonomous "27/27" smoke had only tested HOST-driven grabs → the client-grab-release
path was never exercised (the gap that hid it).

## 2. LMB hard-throw — VERIFIED (commit `eb85ddfb`, PUSHED)
**Symptom (hands-on 12:35):** the client could E-drop a carried pile but **LMB did nothing** (the base-game
whoosh-throw).
**RE (bytecode, pinned):** VOTV's throw is a SEPARATE input from E. E=`InpActEvt_use` (grab + E-drop). LMB=
`InpActEvt_fire` → `throwHoldingProp` → `throwShit(velocity)`. The applied velocity (from `traceThrow` →
`PredictProjectilePath` → `SuggestProjectileVelocity`, which round-trips to the seed) =
**`cameraForward * (15000 / max(objMass,10)) + playerVelocity`, NO cap** (E-drop is capped 650 cm/s; LMB up
to ~1500). The client holds only a proxy (host-authoritative) → native `throwHoldingProp` no-ops + we never
routed it → LMB dead.
**Fix (Variant B — native fidelity):** client PRE-observes `InpActEvt_fire` (_58/_59, both registered; the
`ClientCarryEid` gate + the optimistic carry-clear self-disambiguate press/release), and while carrying sends
`ThrowIntent(mode=hardThrow, camFwd)` with its INSTANTANEOUS camera-forward; the host applies the native
formula with the REAL clump mass (new `ue_wrap::engine::GetActorRootMass`) + puppet velocity, no cap. E-drop
stays `mode=release`, untouched. `ThrowIntentPayload` grew 8→20 (mode + camFwd unit vec). Audit SHIP.
**VERIFIED 13:53:** LMB throws the carried pile where the camera looks at native speed; E-drop stays soft; the
two mechanics coexist.

## 3. The join-window bind-vs-live-mutation CLASS (#1 + #2)
**Unifying root (RE, 2 agents):** the save-identity bind blindly trusts the save-loaded native's position/form
is the eid's CURRENT authoritative state. But if the HOST mutated that eid DURING the client join window
(turned a kerfur ON / moved a pile), the bind cements STALE state and defeats the existing reconcile/convert
machinery. Clean-join + post-join grab/throw are unaffected (the bug needs an in-window host mutation, which
the autonomous clean smoke never does). This is the documented join-window race class
([[feedback-snapshot-before-state-ready]] / [[feedback-fix-then-generalize-mirror-identity]]).

### #1 kerfur turn-on dup — VERIFIED (commit `39a381b0`, push HELD)
A host kerfur turn-ON drops its `KerfurConvert` reliable-send mid-join (the deliberate **B2 pre-world gate**,
`session.cpp:254` — `KerfurConvert` not on `IsPreWorldSendableKind`; no retry BY DESIGN — so "retry the send"
is a crutch that fights the gate). The fallback (a quiescence retire sweep armed from the NPC EntitySpawn
channel) used to exclude ALL Prop mirrors; THIS session's bind binds the stale off-prop as a host-range mirror
→ it was protected → survived (`sweep-retire 0 of 1`).
**Fix:** `CollectLocalOffPropKerfurs` excludes only NON-bound mirrors (`!IsBoundMirrorNative`) so bound mirrors
become retire candidates; the matched bound mirror is torn down MIRROR-aware (`ResolveMirrorEidByActor` →
`UnmarkKnownKeyedProp` → `DestroyActor` → `ElementDeleter` `MirrorManager<Prop>::Take(eid)`) — a plain
`UnmarkAndDestroy` would dangle the mirror Element (`UnmarkKnownKeyedProp` resolves the LOCAL reverse-map only,
early-returns for a mirror). Isolated: the pending-retire is armed ONLY on a host turn-ON, keyed by position,
so a still-off bound kerfur never matches; clean-join → empty pending → no-op.
**VERIFIED 15:42:** `retired bound-mirror stale off-prop ... eid=3148 (mirror-aware teardown)` + `sweep-retire
1 of 1` (was 0 of 1). No extra kerfur.

### #2 pile-move dup — VERIFIED dup+identity (commit `acc416eb`, push HELD); positional polish = b2 (DESIGN)
A host grab+move of a chipPile in-window broadcasts ToClump/ToPile; the client OnConvert spawns a proxy.
The bind's caseII proxy block ALWAYS retired that proxy + bound the native@OLD save position → cemented the
stale position; a later overflow native coexisted → two piles.
**Fix (b1):** in the caseII proxy block, discriminate by `CtxForEid(E)`. `CtxForEid>0` (a PropConvert was
adopted for E → the pile was moved in-window) → the PROXY is authoritative → retire the redundant save-loaded
native, keep E bound to the proxy. `CtxForEid==0` (a fresh host-PropSpawn proxy, no convert) → the existing X
item-4 path (native wins) is unchanged. Inverse of the morph hand-off (#3) for the converted save-loaded case.
**VERIFIED 15:42:** `PROXY-WINS ... case(ii)-converted ... ctx=2`, bind 874/874, **chipPile overflow=0** (12:02
had it → b1 eliminated the overflow-native that was the 12:02 dup's second pile). Dup gone, identity correct.

#### #2 positional nuance + b2 (AS-BUILT, commit `2829ce6d` — hands-on PENDING)
After b1 the moved pile renders as a proxy. The in-window ToPile convert carries BOTH the **moved** position
(`locX/Y/Z`, e.g. host `BROADCAST ToPile ... at (1403.8,-442.3,...)`) AND the **old save-time key**
(`matchX/Y/Z`, docs/piles/09). `SpawnProxy` is given `loc=moved` (E::SpawnActor at the spawn transform), and
nothing repositions it afterward (a landed pile streams no resting pose) — so per the code the proxy SHOULD be
at moved. The user observed a divergence (pile appeared at the old place, then converged when grabbed). **The
exact divergence was NOT pinnable from the 15:42 logs:** the convert-beat-spawn path (`isProxy=0`) logged NO
position-verify (unlike the re-skin path's `ToPile SNAP ... drift=` line). **b2 (AS-BUILT, `2829ce6d`):** in the
`IsTrashProxyClass` convert-beat-spawn branch of `remote_prop.cpp` OnConvert (where `loc` is available, NOT the
bind), gated `proxy && !wantClump` (LAND only), an explicit `SetActorLocation/SetActorRotation(proxy,{loc/rot})`
+ read-back `[PILE] CLIENT ToPile SNAP(spawn-on-convert) ... drift=Xcm` (mirrors the re-skin snap ~:1018). Forces
the moved position (belt-and-suspenders vs a static-mobility no-op) AND proves it next hands-on (drift≈0 → was
already moved, divergence is physics-settle/perception → **b2.1** host-streams-settled-pos; drift>0 → the snap
fixed it). Isolated to the in-window LAND-beat-spawn case; carry / clean-join / morphBoundNative hand-off excluded
by the gate; interaction-convergence stays as the fallback. Audit SHIP (event-driven clone of the audited re-skin
block). Builds Release, deployed + hash-verified `1155147789AA`. **NOT a guaranteed fix — an attempt +
observability** until the next run's drift log; #1/#2 (no dup, identity) already VERIFIED and must stay PASS.

## 4. Verification-gap lesson (recurring)
Every bug this session was invisible to the autonomous smoke because the smoke does CLEAN joins + HOST-driven
grabs and is rendering-blind. The client-grab-release (#3), the LMB input (#2-throw), and the in-window host
mutation (#1/#2) are all hands-on-only. The autonomous "27/27 / 874/874 VERIFIED" was real but PARTIAL —
it never covered these paths. See [[feedback-interaction-smoke-not-join-smoke]].

## Deployed / committed (end of session)
- origin/main `eb85ddfb` (grab/throw + stable-id stack, pushed + hands-on verified).
- HEAD `2829ce6d` = `eb85ddfb` + #1 `39a381b0` + #2 `acc416eb` + docs `22f15f84` + b2 `2829ce6d` (4 ahead,
  push HELD pending the b2 hands-on).
- Deployed + hash-verified `1155147789AA` (Release, b2) host+client+dev (was `FE87964A893A` pre-b2). `[dev]`
  ini: `save_identity_bind=1` + `save_identity_map_log=1`.
- NEXT: ONE hands-on (b2 re-run) — kerfur turn-on + pile move in window (runbook
  `research/handson_runbook_2026-06-26_joinwindow_1_2.md`, updated for b2); confirm the moved pile renders at the
  MOVED position + read the `ToPile SNAP(spawn-on-convert) drift=` log → push the #2 rubicon WHOLE
  (#1/#2/docs/b2). If 2b fails with drift≈0 → b2.1 (host streams settled pos). Then optional (c) host-gate
  generalization (defer all in-window mutations until joiner quiescence — larger, separate).
