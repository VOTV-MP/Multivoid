# Hands-on runbook — G1 (CallFunction-route capture) + TryCapture decline/provenance

**Date:** 2026-07-14 pm · **Take:** G1 observe increment (still OBSERVE-ONLY -- no behavior change,
no capture/suppress/converge; the conversion verbs run unmodified).
**Deployed DLL SHA256 (first 16):** `e09044d77a7863e7` -- byte-identical on HOST, CLIENT_1, CLIENT_2, DEV
(verified). Supersedes `1E41A807`.
**Flags:** `vm_dispatch_log=1` on HOST + CLIENT_1 (`gnatives_probe=0`). Set already.

---

## WHY this run exists (what the last archaeology pinned)

A chain of measurement this session overturned two of my own wrong conclusions and pinned the real bug:

- **bug1's relay is the generic `grab_hook[destroy-seam]` CLIENT broadcast** (`key='...' eid=0`), fired on
  every client turn-on when the local mirror prop dies. Host resolves by key -> kills its authoritative
  prop -> the follow-up convert-request finds "no live prop -- dropped." (take9_client:24181,
  take10_client:24981+5, obs_client:2 -- six-for-six.)
- **`TryCaptureKerfurPropDestroy` is the guard sitting on that relay** (`prop_destroy_seam.cpp:118`) and it
  is **STARVED, not dead**: it returns false because its B-discovery (a fresh-NPC stamp + 500cm proximity)
  finds nothing on the client. (An earlier "it's structurally dead" claim was a FALSE-NEGATIVE grep -- the
  destroy-seam log line prints no class name, so `grep .*kerfur` was blind. Retracted.)
- **The destroy that fires the relay is the conversion verb's OWN in-window self-destroy** (measured:
  obs_client destroy-seam relays correlate by actor pointer to the assembler's `IN-WINDOW self` destroys,
  verbId=2). So `K2_DestroyActor(self)` IS Func-visible; only the VERB (spawnKerfuro) is EX_Local.

**The fix this is de-risking:** feed the deterministic in-window captured-B into the starved guard so it
fires and suppresses the relay (client) + converges (host) -- the foundation already targets
`ConvergeAfterConversion(capturedForm)`. Before wiring it, two things are unmeasured:

- **G1:** the assembler's capture is only proven on the LOCAL toggle (0x45 bracket). The dominant
  host-executes-client-request path runs the verb via `R::CallFunction`, which is **0x45-blind** -- never
  observed. This build adds `g_requestVerbEid` as a SECOND capture scope so a CallFunction-route B/destroy
  is counted (`formInReqScope` / `selfInReqScope`) instead of vanishing into `formOut`/`kerfurOut`.
- **The decline reason:** this build logs WHY `TryCapture` returns false (stamp-starved vs saw-B-but-no-
  match) + the destroy's provenance (is a 0x45 verb open / is it the self-destroy / is a request executing).

---

## PRECONDITIONS

1. No VOTV running. Use the HOST copy + the CLIENT_1 copy (both have `vm_dispatch_log=1`).
2. **Fresh save -- New Game** on the host (never a stale slot).
3. At least one kerfur available (spawn one if the fresh world has none). A floppy disc is optional here.

---

## STEPS -- FORCE ALL FOUR CELLS (this is the whole point; each cell is a distinct code path)

Do each deliberately, wait ~2 s between toggles. Keep a rough tally.

**CELL 1 -- HOST-OWN turn-ON (bug2; ZERO converge fire-line in any prior log -- the never-exercised cell)**
1. On the HOST, with a kerfur currently a **prop** (turned off), turn it **ON** yourself (host player
   operates the radial menu). This is the local 0x45 route on the host. Repeat 2-3x.

**CELL 2 -- HOST-OWN turn-OFF (the known-good express-edge)**
2. On the HOST, turn a live kerfur **OFF**. Repeat 2-3x.

**CELL 3 -- CLIENT turn-ON (the live bug1/drop symptom + the G1 CallFunction route on the host)**
3. On the CLIENT, turn a kerfur (currently a prop) **ON**. Repeat 2-3x. (This is the one that dropped
   before -- watch whether the kerfur ends up an NPC on BOTH peers or vanishes.)

**CELL 4 -- CLIENT turn-OFF**
4. On the CLIENT, turn a live kerfur **OFF**. Repeat 2-3x.

**END:** disconnect the client, then quit both. Disconnect dumps the two assembler summary lines.

(Copy both `votv-coop.log` files to the scratchpad BEFORE relaunching anything -- the log truncates at boot.
Or just leave the games closed and tell me; I'll copy them.)

---

## WHAT I READ AFTER (you can just hand me the logs)

Logs: `Game_0.9.0n_HOST\...\Saved\Logs\votv-coop.log` + `Game_0.9.0n_CLIENT_1\...\Saved\Logs\votv-coop.log`.

**G1 -- the CallFunction route (host log, session-end CONTAINMENT SUMMARY):**
```
[kerfur_asm][HOST] CONTAINMENT SUMMARY (...): ... spawn{formIn=.. formInReqScope=.. formOut=.. ...}
    destroy{selfIn=.. selfInReqScope=.. otherIn=.. kerfurOut=..} ...
```
- **`formInReqScope > 0`** = GREEN: the CallFunction-route B (host executing a CLIENT's turn-on/off) IS
  seen at FinishSpawningActor while the request eid is live -> the capture is viable on this route, not
  just the local 0x45 one. **`formInReqScope == 0` while CELL 3/4 ran** = the CallFunction route does NOT
  spawn B through the Func-visible FinishSpawningActor (the borrowed-proof risk realized) -> HALT, the fix
  needs a different B-source for that route.

**The decline reason + provenance (both logs, per client/host turn-on):**
```
kerfur_convert: TryCapture DECLINE (CLIENT) actor=.. -- STAMP LIST EMPTY (client stamp-starved?) --
    relay proceeds. provenance{vmActive=1 verbId=2 ctxSelf=1 reqEid=-1}
```
- `STAMP LIST EMPTY` on the client = the starvation root confirmed: `NoteFreshKerfurNpcSpawn` never stamped
  (host_spawn_watcher seam host-only?) -> feed the guard the assembler's captured-B instead (the fix).
- `.. fresh stamp(s) but NONE qualified` = the guard saw B but the proximity/ownership match failed (the
  take-8/10 misfilter class) -> deterministic capture also fixes it.
- **`provenance{vmActive=1 ctxSelf=1}`** = the destroy IS the conversion verb's own self-destroy (0x45
  bracket open, dying actor == the verb Context) -- confirms the seam sees the GAME's conversion destroy,
  not our teardown. `reqEid>=0` = the CallFunction (host-exec-request) route. `vmActive=0 reqEid=-1` on a
  kerfur-prop destroy that still relays = a DIFFERENT destroy (teardown) -- would reopen the provenance q.

**The relay itself (correlate):** `grab_hook[destroy-seam]: CLIENT broadcasting DESTROY key='..' eid=0`
one line before each client `POLL turn-on` = bug1's relay still firing (expected -- this build does NOT fix
it, it measures why the guard didn't stop it).

**bug2 (CELL 1, host-own turn-ON):** look for ANY converge on the host for a host-own turn-on -- a
`BindFormActor ..->NPC(turn-on)` NOT preceded by a `HOST executing turn-on .. from slot` line (slot = a
client request). If a host-own turn-on produces NO converge at all, bug2 is confirmed live (its only prior
"fix" was the starved guard).

---

## HONEST STATUS

- **Observe-only.** No behavior changed; the conversion verbs + the relay run exactly as before. This build
  MEASURES the two things the fix rests on (CallFunction-route capture viability + the decline root); it
  does NOT fix bug1/bug2 yet.
- **After GREEN** (formInReqScope>0 on the CallFunction route + the decline root confirmed as
  stamp-starvation): the fix is a narrow which-B wire -- feed the assembler's in-window captured-B to
  `TryCaptureKerfurPropDestroy` (client suppress + host converge) and to `ConvergeAfterConversion` on the
  request route, retiring only the proximity search. RETIRE NOTHING ELSE (POLL + TryAdopt stay -- the only
  demonstrably-converging paths). bug2 gets its converge from the local 0x45 route by the same capture.
- **After any HALT** (formInReqScope=0, or the decline is NOT starvation, or a teardown-provenance relay):
  the fix's B-source assumption is wrong on that route -- re-root before wiring.
