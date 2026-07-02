# Hands-on runbook 2026-07-03 — THE RE-BIND THREAD (dup pile + 4 Hz drain root fix)

**Deployed:** DLL `77BB5D58CFB15505` on all 4 installs (hash-verified). Protocol UNCHANGED v94 —
mixed old/new peers are wire-compatible, but BOTH peers should run the new DLL (the fix has host
AND client halves). Paks unchanged (hl_einstein `AE49002C`, rvi `ED666BE5`).

**Autonomous smoke:** PASS (fresh client vs s_1234 host, 12 s past join, kill clean). Evidence:
626 join-window twins converged in TWO drain passes (dead-premise drops; the old code burned 40
passes x 250 ms each); the eid=2869 deferred destroy dropped LOUD at the 8-pass cap; queues empty
at kill; no RAM breach; no [Error]/SEH in either log.

## What was broken (your 20:27 report, "клиент взаимодействует с дюп пайлом своим локальным")

The 20:24-27 client log proved ONE root with three faces: when the host moved a pile during your
join, the client recorded the move by OVERWRITING the pile's identity key with the new position.
But when the join-window purge re-created the client's save natives, the game spawned them at the
SAVE position (the game replays its own save arrays — it knows nothing about the host's move). The
re-bind then searched the NEW position forever, never found the re-create, and:
1. the ownerless native at the old spot became your client-only dup (native grab, no GrabIntent,
   invisible to the host);
2. the armed position-correction waited for a bind that could never happen — and corrections had
   NO retry cap — so the reconcile drain ran at 4 Hz forever (your unstable-FPS leg 2).
Two more latent bugs fell out of the same forensics: raw IsLive on freed row pointers misread
liveness (permanently blocking kerfur re-binds), and the wrong-class deny heal you got in take-4
was a client-side NO-OP (the re-assert was silently rejected as a duplicate eid).

## The fix (one commit, five seams — all host+client symmetric, no wire change)

1. **Two positions per pile identity**: the save position is now IMMUTABLE (where re-creates
   spawn); the host's current position is a separate overlay. The re-bind searches the host pos
   first (a churn survivor lives there), then the save pos (a purge re-create spawns there) — and a
   save-pos re-bind immediately schedules the snap to the host pos in the same drain pass.
2. **IsLiveByIndex everywhere** an element-held actor pointer is tested (freed-memory misreads).
3. **Twins retire ONLY on positive evidence** (E bound to a live actor; host-vacate included). The
   old "unconfirmed retired when under 50%" arm is GONE — it could destroy E's own re-create one
   step before the re-bind claimed it. Dead-premise twins (pile back at its old spot) drop
   immediately instead of spamming 10 s of walks.
4. **Everything in the drain is now pass-bounded** (twins 40, destroys 8, pos-corrections 40,
   kerfur retires 40) — no residual shape can ever pin the 4 Hz drain again; every drop is a loud
   `[WARN]` line naming the eid.
5. **Keyed identity survives actor churn**: the join sweep re-binds a re-created keyed prop onto
   its orphaned row (claim transferred) instead of dooming it (the 19:24 eid=2947 wedge upstream);
   a host re-assert PropSpawn now actually re-points a smeared row (the take-4 deny heal receive
   half — eid=3129 class); a kerfur native whose eid row holds a foreign actor (address recycle)
   re-binds by key, never destroying the foreign actor.

## Your tests (host + client, both on `77BB5D58`)

1. **THE 20:27 repro — mass-move during join**: host: stand at a pile cluster, start
   grab-throwing piles across the room WHILE the client menu-joins; keep moving them through the
   client's whole load. Client after load: the cluster must show NO leftover piles at the old
   spots, every pile grabbable, grabs visible to the host. Client log MUST show:
   `RE-BIND chipPile by position` / `by HOST pos` lines + `[PILE-B3] CLIENT pos-correction APPLIED`
   (drift~0) + `[PILE-1C]` twin lines going to `0 still pending` within seconds — and NO
   `quiescence_drain: steady-state reconcile` lines still repeating 2+ minutes after the join.
2. **FPS leg 2**: after test 1 settles, play piles 10+ min. The 4 Hz `[HITCH-SRC] net_pump::Tick`
   storm must be absent from the client log in steady state. If ANY `[PILE-B3] ... dropping
   pos-correction` or `kerfur_reconcile: dropping pending off-prop retire` WARN appears — the
   bounded backstop fired where the re-bind should have won; report it with the eid (it means a
   shape I haven't seen).
3. **Wedged pile heal (eid=3129 class)**: if you find a GUI-but-ungrabbable pile: press E, wait
   ~2 s, press E again — the host log shows `re-asserting the authoritative row`, the client log
   must now show `HOST RE-ASSERT rebound row` (NOT silence — that was the NO-OP), and the pile
   must become grabbable without a host restart.
4. **Kerfur off-props after a churny join**: any kerfur that was OFF at join must be exactly ONE
   actor on the client (no side-by-side twin), toggleable by the host, state visible to you.
   Client log: check `unbound-kerfur reasons` — `no-map-entry` entries are benign; a repeating
   `WRONG-OCCUPANT heal` WARN for the same eid every pass would be a loop — report it.

## Known observation for a FUTURE thread (not this one)

The autonomous smoke (fresh client world) shows `4 unbound kerfur ... 4 same-key duplicate (seam
dedup owns)` — four client kerfur natives whose eids are bound to OTHER live actors with the SAME
key. Pre-existing (the 20:24 log had the same 5 as silent "unbound kerfur" walks); my change only
made the reason visible. If you see kerfur off-prop DOUBLES in-world on the client, that's this —
it has its own thread now that the log names it.
