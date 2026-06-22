> SUPERSEDED (2026-06-22) -- this take's work is FIXED + verified; see research/handson_runbook_2026-06-22_regression_and_harness.md + the canonical finding.

# Hands-on runbook — CLOSE-B chipPile carry — take-26 (the carry fix; YOUR hands-on is the test)

**Deployed:** `votv-coop.dll` SHA **`C9F28176`** to all 4 copies (host / copy / copy2 / dev). Proto **v82**
(UNCHANGED). Build CLEAN (Release). **NOT autonomously smoked** — you are on the PC, so this is prepared
ground and YOUR hands-on is the test.

> **Update `EE0DD83C` — the `!carrying` gate (live) + a READ-ONLY `simulateDrop` thunk.** Logs proved the
> freeze was NOT the re-pile/`HasPendingSettle` model: the held-puppet is RECREATED by `updateHold` (`heldActor`
> ptr changes, `pendingSettle=0`), so `HasPendingSettle` structurally couldn't catch it; the spurious release →
> ctx churn → the client held carry poses (`ctx` ahead of `known`) → frozen. The carry data path itself is
> WHOLE (host emits → client drives the eid=5625 proxy → a Movable actor). FIX (live in this build): the
> release edge now suppresses the WHOLE carry (`!carrying`); the latch closes via the land-settle (drop) — and,
> after the FLIP, via the **`simulateDrop` Func-thunk** (the named real drop/throw seam, distinct from
> `updateHold`; `throwHoldingProp` routes through it so it catches throws too). This build wires the thunk
> **READ-ONLY (logs only)** to PROVE it fires for the clump before the close depends on it.
>
> **This run tests TWO things at once:** (a) **the carry flicker is GONE** (`!carrying` is live — the main
> check); (b) **`[SIM-DROP]` fires on drop AND throw with `carrying=<the carry-eid>`** (the seam proof). In
> this read-only build a **throw still teleports** (the thunk only logs; the latch closes via the settle, no
> velocity) — that's expected, it's restored at the flip.
>
> **HANDS-ON RESULT (EE0DD83C, 2026-06-22):** the **carry-FREEZE is GONE — VERIFIED [V]** (the clump updates
> through the carry; `!carrying` holds). THREE open issues, all root-found from log+code (see the canonical
> finding `votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`): (1) carry **JANKY** — the clump pose
> streams `key.len=4` (its BP `GetKey`), receiver routes key-first off the eid proxy → no `drive #` → the proxy
> moves only on converts; FIX = force the clump pose keyless. (2) proxy **SMALLER** — scale never sent
> (`BroadcastConvert`) nor applied (`trash_proxy`); FIX = send+apply `GetActorScale3D` per-form. (3)
> intermittent **ORPHAN** dup — did NOT reproduce (zero `isProxy=0`); needs an instance. **NEXT BUILD = #1
> (keyless) + #3a (scale)**; a fresh runbook (take-28) follows. The `[SIM-DROP]` lines for the throw-flip are
> still wanted, parked behind jank+scale.

## What changed — CLOSE-B (host-side carry latch + land-settle)
The host's stock chipPile churn — a held clump re-piles on cluster contact ~1/s and the game auto-re-grabs —
was being broadcast convert-by-convert, so the client re-skinned its ONE proxy to a PILE every cycle and
teleported it back to the grab spot (`remote_prop.cpp:1148`). That was BOTH the 2 fps AND the
"old pile won't disappear" — one root. CLOSE-B (`trash_channel.cpp`): a per-eid latch OPENs on the real grab,
SUPPRESSES the churn re-pile broadcasts + ctx bumps, REBINDS each churn re-grab onto the carried eid (so the
carry stream stays alive), and CLOSEs on the real land via a settle (a re-pile NOT followed by a re-grab
within K=6 ticks = the land → broadcast it once). Files: `trash_channel.{h,cpp}`, `local_streams.cpp`
(held-edge re-grab rebind), `subsystems.cpp` (`TickCarry`).

## The test — grab a pile NEAR A CLUSTER, carry, throw (host grabs; client watches)
The churn only fires while the held clump touches other piles, so **test in a dense pile cluster** (the
"staying near piles" case you reported). Host grabs; watch the CLIENT screen.

1. **Carry near the cluster (the main test).** Host taps E on a pile in a cluster, carries it around AT the
   cluster for ~10 s. On the CLIENT: the clump should **follow CONTINUOUSLY and smoothly** — the key check is
   that the position is **NOT frozen between E-presses** (that frozen-between-E behaviour was the `65AD883A`
   release-flicker, fixed in `C9F28176`); it should track the host the whole carry. Also: **ONE clump form**
   the whole time (no pile↔clump flicker), and the **grab spot should EMPTY** (the proxy follows the host away).
2. **Drop / throw.** Host drops or throws the clump. On the CLIENT: exactly **ONE** clump→pile morph at the
   landing spot, ~50–100 ms after the host's (normal client-behind-host latency).
3. **Two piles (per-eid).** Grab one pile, drop it, immediately grab a different one — the two should not
   interfere (independent latches).

## Acceptance — GREEN vs the two failure modes (distinguish in the log, don't lump as "broken")
- **GREEN** = smooth carry + one form the whole carry + one land-morph ~50–100 ms + grab spot empties.
- **K too small** → a brief `clump→pile→clump` flicker **DURING CARRY** (at churn cycles), self-correcting.
  This is **NOT a bug — raise K** (see tuning below). The drop itself still gives one morph.
- **Broken CLOSE** (FAILURE) → after a drop the clump **stays a clump forever** on the client (latch
  stuck open / land never committed).
- **Broken suppression** (FAILURE) → the old **0.5–2 fps / stuck pile** returns during carry (converts not
  being gated).

## Read the HOST log (`...\Game_0.9.0n\...\Win64\votv-coop.log`) — greps
- `Select-String "TRASH-CH"` — the whole latch trace.
- `Select-String "TRASH-CH.*carry OPEN"` — the grab opened the latch (once per real grab).
- `Select-String "TRASH-CH.*land-settle START"` — re-piles being HELD (the churn, suppressed).
- `Select-String "TRASH-CH.*CANCEL eid=.* gap="` — **the ToPile→ToClump re-grab gap** = the churn re-grab
  latency. **Read `gap=N` and tell me the max** → I set K = max(gap)+margin (default is 6). Many CANCELs with
  small gaps = healthy churn-folding.
- `Select-String "TRASH-CH.*LAND COMMIT"` — **CLOSE fired** (should be exactly ONE per real drop/throw).
  **Its ABSENCE after a drop = the stuck-latch FAILURE.**

## Read the CLIENT log — the convert stream should be quiet now
- `Select-String "PILE. CLIENT recv convert"` — during a carry you should now see **ONE ToClump (grab) + ONE
  ToPile (land)** for the eid, **NOT** a stream of `ToPile` every second (that stream was the bug).

## v1-known (report if seen; these are the queued v2 items, not regressions to chase now)
- **Throw arc:** if the host's churn flickers `holding_actor` empty for a frame, the existing release edge may
  send a spurious PropRelease mid-carry (a brief proxy twitch). If you see the carried clump **drop/twitch**
  without you releasing → note it; v2 gates the release edge by the carry latch.
- **Safety:** the on-DESTROY `ForgetEid` hook is v2 (a latch on a despawned eid is benign — the existing
  PropDestroy still cleans the client proxy). A disconnect clears everything.

## Honest status
- CLOSE-B v1: **built CLEAN, audit CLEAN, deployed `65AD883A` to all 4 copies, proto v82**. **NOT verified** —
  no autonomous smoke (you're on the PC); your hands-on is the test. Root + design:
  `research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`.
- After your run: tell me (a) GREEN or which failure mode, (b) the `gap=N` values to tune K, (c) whether the
  grab spot empties. Then I tune K / fix, commit-verified, and queue v2 (throw-arc + safety hook).
