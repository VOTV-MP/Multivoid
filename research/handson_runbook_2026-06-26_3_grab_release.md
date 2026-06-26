# Hands-on runbook — #3 grab-release fix (client-grab native pile + throw)

**Date:** 2026-06-26
**Deployed DLL:** SHA256 head `0849F954E435` — hash-verified MATCH on HOST + CLIENT + DEV.
**Change:** 1 line. `remote_prop.cpp:1090` — the morph hand-off branch now calls
`NoteClientConvertObserved(E, wantClump)` before `return proxy`, exactly like the
IsProxy branch at :1039. This arms the client carry-latch (`g_clientCarry`) when you
grab a bound save-loaded NATIVE pile, so the existing THROW toggle can fire.
**push:** HELD. **Bind:** unchanged (874/874 works). `[dev] save_identity_bind=1 + save_identity_map_log=1` set on both.

---

## Why this test (closes the gap that hid the bug)
The autonomous "27/27 PASS" only tested **host-driven** grabs. The **client grabbing a
native pile + throwing** was never tested — that path had no release. This runbook tests
exactly that path.

## What to do
1. Launch `mp_host_game.bat` (host) — load your usual pile-heavy save (the one with ~870 chipPiles).
2. Launch `mp_client_connect.bat` (client) — let it fully join (wait for the world to settle, ~10s).
   - You do NOT need to toggle kerfurs or move piles in the connect window for this test
     (that was #1/#2 — still open, separate fix). Just join clean.
3. On the **client**, walk up to any chipPile. Aim directly at it so the grab prompt shows
   (it's a real native now — lookAtActor, occlusion-correct). Press **E** to grab.
4. Press **E** again to **throw** it.
5. Walk to a **different** pile, aim, press **E** to grab it. (This is the critical step — proves
   the host slot freed.)
6. Optionally throw the second one too. Then you can close both windows.

## Acceptance — the 4 checks (all must hold)
Read both logs after:
- CLIENT: `Game_0.9.0n_copy\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`
- HOST:   `Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`

| # | What | Where to look | PASS = |
|---|------|---------------|--------|
| 1 | Carry CONFIRMED on the native grab | CLIENT log, right after `[GRAB-INTENT] CLIENT SENT eid=<N>` | a line **`[GRAB-INTENT] CLIENT carry CONFIRMED eid=<N>`** appears (this is what was MISSING at 12:02) |
| 2 | Throw fires | CLIENT log | `[THROW-INTENT] CLIENT ... requesting throw from host` + `CLIENT SENT eid=<N> -> host (carry released)` |
| 3 | Carry ends on land | CLIENT log | `[THROW-INTENT] CLIENT carry ENDED eid=<N> (inbound ToPile -- re-piled)` |
| 4 | Slot freed, next grab passes | HOST log | the SECOND grab is `[GRAB-INTENT] EXEC ... SUCCESS` — **NOT** `DENIED ... slot already holds eid=<N>` |

If #1 appears and #4 passes (second grab succeeds, no DENIED-stuck) → **#3 verified**.

## FAIL signature (the 12:02 bug, for reference)
- No `carry CONFIRMED eid=<N>` line after the native grab.
- `[TRASH-CARRY] CLIENT APPLY eid=<N>` repeating every second forever (stuck carry).
- HOST `[GRAB-INTENT] DENIED eid=<other> -- slot already holds eid=<N>` on the next grab.

## What this does NOT cover (still open — next)
- **#1** (extra lying-down kerfur): host KerfurConvert reliable-send failed in the join window, no retry.
- **#2** (pile dup on a window-moved pile): convert arrived before bind (`known=0 holds forever`) + overflow.
Both are the join-window save-vs-live race — a separate fix after #3 is confirmed.
