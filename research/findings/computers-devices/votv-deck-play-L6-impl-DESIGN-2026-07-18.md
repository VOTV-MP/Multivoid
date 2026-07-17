# L6 deck playback (PlayDeckEvent=107, v117) — impl design of record (2026-07-18)

7-round `/qf` impl pass (converged: a genuine critic "that holds" at R7/15; thread:
scratchpad `qf_thread.md`, archived on the next topic). Arch frame:
`votv-signal-chain-all-units-DESIGN-2026-07-16.md` §L6 (which this REVISES — see
"Deviations from the arch doc"). Fact bases: `votv-signal-chain-units-RE-2026-07-16.md` §3
+ the fresh bytecode censuses below. Status: **BUILT; smoke x2 PASS (30 s + 75 s,
2026-07-18 20:48/20:51) with the e2e self-test chain proven end-to-end** — host
"SELFTEST activate fired" -> "organic play idx=0 gen=1" -> client "gates failed
(activePlay=1 count=0) -- skipped" (the designed WARN, empty smoke deck) -> host
"organic stop gen=1" -> client "stale stop gen=1 dropped (applied=0)" (the GEN GUARD
firing). Probe-first verdict: ambient Deactivate = **1563/60 s (~26/s game-wide)**,
miss-path = flag-check + one pointer compare => negligible; sigHits=2 = exactly the two
self-test edges. Zero unexpected WARN/ERROR on either peer. NOT hands-on (the audio
positive is the take's).

## The measured fact base (all fresh this session)

| # | Fact | Evidence |
|---|---|---|
| F1 | playSignal()/stopSound()/fin() are PARAMETERLESS UFunctions on AanalogDScreenTest_C; signalSound = UAudioComponent* @0x570 — the SAME class as the v115 audio seam | SDK CXXHeaderDump analogDScreenTest.hpp:597-600, :112 |
| F2 | Whole-asset call census on signalSound: **Activate x1** (playSignal body, operand **EX_True => bReset=TRUE**, restart forced), **Deactivate x1** (stopSound body), SetSound x1, IsPlaying x1 (LIVE component read, button region). All EX_VirtualFunction/EX_FinalFunction on NATIVE callees -> Func-seam visible (v115 doctrine: callee nativeness decides) | Imports-resolved bytecode walk of research/bp_reflection/analogDScreenTest.json |
| F3 | Deactivate sites on the six v115 whitelist comps: **ZERO** (loops stop via SetActive(false), already forwarded); eff_pf/eff_ff Deactivates exist (updToggles) but are not whitelist comps | same census |
| F4 | stopSound callers x5 (button toggle, fin, active_play setter, IMPORT, EXPORT); playSignal callers x1 (button); **cross-BP callers: none** (ui_consolesAtlas census: zero) | caller census both assets |
| F5 | fin has **ZERO direct BP callers** — only EX_BindDelegate:fin -> signalSound.OnAudioFinished => delegate-only dispatch (PE-visible per doctrine line 30 — INFERRED; the design is decoupled from it, see gen guard) | delegate-op census |
| F6 | play_volume/play_selectIndex/active_play already ride DeskInput (v112) with guarded applies + per-field echo-prime; DeskState adopt seeds them at join; desk poll = 250 ms | protocol.h:4305-4327, 3241-3264; desk_input_sync.cpp:25,228-237 |
| F7 | playSignal/stopSound write **NONE of the 14 DeskInput-polled fields** (no poll-back after a wire apply); active_play writes = EX_LetBool x1 (the toggle handler); the game itself raw-writes play_selectIndex (EX_Let x2 scroll handlers, no setter) | write census |
| F8 | The ActivePlay apply side effect = UNCONDITIONAL stopSound both directions (console_desk.cpp ApplyActiveToggleEffects unit 0) — can only STOP playback, never start => no second play author | code + census |
| F9 | ALL involved kinds (DeskState/DeskInput/DeskScanEvent/SavedSignalAppend/Delete + default) = Lane::Normal => one ordered host->peer stream; append-before-play holds topologically (a presser can only select a row it already received) | session_lanes.h |
| F10 | Deck rows enter ONLY via saveSignal (gates decoded>=size at creation) + cross verbatim via append chunks => rows byte-identical across peers => playSignal's decoded gate CANNOT diverge | saved_signals.h RE + signal_sync |
| F11 | Native C++ teardown calls bypass the Func thunk (vtable-direct); the seam sees only BP/reflection dispatch | v115 model (a whitelist seam would otherwise catch engine-internal audio constantly) |
| F12 | Both v115 seam consumers early-out on the shared g_wireApplyDepth (desk_snd_fx.cpp:104,114); ufunction_hook chains multiple cbs per UFunction (slot-stamped thunks; second install's original = the first thunk) | code |

## The design (as built)

1. **Detection at the v115 Func seam, not the arch doc's 250 ms poll** (the arch doc's own
   tier rule: seam > poll; the seam exists on this exact class since v115). The 4th Func
   patch: ActorComponent:Deactivate. Routing: `IsSignalSound(comp)` pointer-first;
   ALL non-signalSound Deactivates dropped (F3: no whitelist comp is ever Deactivate'd —
   zero v115 behavior change). signalSound = a **7th slot in desk_audio's existing comp
   table** (same g_compsValid invalidation + IsLiveByIndex refresh, ONE re-resolve owner;
   an alias with the whitelist refuses the cache with a WARN — dead-guard discipline).
2. **Edges (latch-free):** organic Activate(signalSound) -> broadcast {play, selectIndex,
   gen}; organic Deactivate -> {stop, gen} unless inside the fin PE bracket. Wire-applied
   edges swallowed by the shared ScopedWireApply. Covered stop paths BY CONSTRUCTION
   (invariant, not a verb list): any peer's stop button (the deck is claim-free world
   buttons — any player may stop, natively), the power-off side effect, IMPORT/EXPORT
   (pre-covers the L5 interplay).
3. **GEN GUARD — correctness independent of fin's PE visibility (F5 is inferred):** the
   play author mints max(seenGen)+1; a stop carries the gen of the playback it terminates;
   receivers drop stale-gen + duplicate stops; plays apply unconditionally and REALIGN
   g_appliedGen. If fin proves EX-dispatched (bracket never fires), every peer's
   natural-end stop is dropped by the guard — no cross-kill of a restarted (higher-gen)
   playback, no eaten edges on reconnect (stops compare to the gen realigned by every play
   apply; a retained host seenGen only raises the mint). The fin bracket
   (RegisterPre/PostObserver on fin) is spam suppression, best-effort.
4. **Wire apply:** pre-check the divergence-CAPABLE gates only — active_play + index <
   saved_signals::Count() (the decoded gate cannot diverge, F10) — on fail WARN + skip
   (one silent track, self-heals; also prevents the mirror's false native deny BEEP);
   selectIndex rides through the **v112 DeskInput apply author** (guarded write +
   echo-prime — closes the scroll-then-play race where the 250 ms poll's delta does not
   exist yet; one authority, two transports); then reflected playSignal() under
   ScopedWireApply (bReset=TRUE => a mid-play apply is a clean restart, F2). Stop -> gen
   check -> stopSound() under the guard.
5. **Wire:** PlayDeckEvent=107 {op u8, selectIndex i32, gen u32} (12 B), Lane::Normal
   (order-coupled with DeskInput + SavedSignalAppend), relay-whitelisted, router case in
   event_dispatch_signal.cpp (the new family), kProtocolVersion 116->117.
6. **JOIN:** no playback seed (arch residual: a joiner misses in-flight playback —
   playSignal has no seek); fields seed via DeskState adopt; the play/stop toggle
   self-heals (live IsPlaying read; the adopt carries no playing bool). **LEAVER:** fin
   self-terminates on every peer natively. **Teardown:** OnDisconnect in the
   subsystems.cpp fanout (every session-end path); broadcasts connected()-armed.
7. **Module:** coop/interactables/deck_play_sync.{h,cpp} + the 7th slot/Deactivate/self-
   test in ue_wrap/desk/desk_audio + CallDeckPlaySignal/CallDeckStopSound/DeckFinFn in
   ue_wrap/desk/console_desk. Single-desk (the v112 precedent).

## Deviations from the arch doc §L6

- **Seam detection replaces the 250 ms bIsActive poll** (the doc's own tier rule outranks
  its L6 line; F2 makes the seam exact).
- **PlayDeckEvent carries gen** (not in the arch sketch) — the R3 addition that decouples
  correctness from fin's unproven PE visibility.
- **Mirrors replay reflected playSignal() with NO selectIndex parameter** (the fn is
  parameterless — F1; the arch line "playSignal(index)" was a sketch): the index rides the
  DeskInput apply author first.

## Instruments (probe-first, qf R5/R6)

- Deactivate seam-fire counter + sigHits, 60 s cadence when nonzero — the v115 always-on
  counter shape (one line/min; NOT the closed-measurement diag-battery class). The smoke
  reads the ambient rate; the take reads the real-world rate. (R6 wording said ini-gated;
  built as always-on to match the shipped v115 precedent line-for-line — strictly more
  observable, same cost class.)
- Dev self-test `[dev] deck_selftest=1` (HOST ini): +25 s reflected organic Activate ->
  +27 s Deactivate on signalSound. Expected evidence: host "SELFTEST activate fired" +
  "organic play idx=.. gen=1" + client "gates failed -- skipped" WARN (fresh save = empty
  deck), then host "organic stop gen=1" + client "stale stop gen=1 dropped (applied=0)".
  Proves patch/routing/classification/wire/apply-path/gen pre-hands-on. **The full
  playSignal audio positive is EXPLICITLY the take's** (a live fin-catch log = the
  "deck_play: seams installed ... finPre=1" line + absent natural-end broadcasts).

## Residuals (documented, accepted)

(a) simultaneous two-peer play press (<~300 ms transit) cross-applies (A ends on B's row
and vice versa) — no claim gates world buttons; gen ties minted concurrently accepted in
arrival order; self-heals at the next press. (b) power-off emits an ActivePlay delta AND a
stop event — idempotent redundancy. (c) joiner misses in-flight playback. (d) the fin flag
is atomic (the PE detour "sometimes a task-graph worker" note). (e) toggle-then-play
inside the 250 ms poll window skips one track (human two-button gesture almost always
slower); self-heals.

## PRODUCT QUESTION (surfaced, non-blocking, default NO)

"Peer X played signal Y" as an activity-feed line? Default NO — the playback is
world-audible at the desk (self-announcing) and a feed line per play/stop is spam-shaped.
One line in deck_play_sync if wanted (the v116 catch-feed precedent).

## /qf round map (the thread's shape)

R1: author-latch DISSOLVED (stale-latch cross-kill + it swallowed a legitimate non-author
stop); the F8 "atlas screen-exit stop" was a MISREAD (the comment describes powerChanged;
atlas census: zero) — REFRAME. R2: Deactivate caller census (drop-all-but-signalSound),
bReset=TRUE measured, one-lane ordering, live IsPlaying. R3: the GEN GUARD (fin-visibility
decoupling), the native-caller model, byte-identical rows, the selectIndex second-transport
justification, residual (e). R4: the write census (no poll-back), single-owner routing
walk, the fresh raw-write check on play_selectIndex. R5: wire-checklist coverage, the
probe-first instrument commitment, the guard walk (both consumers, one depth counter),
teardown/gen-skew derivations. R6: conditional hold -> the three resolutions (one
re-resolve owner + WARN discipline; the dev self-test as the known-positive; the counter
wording). R7: "that holds".
