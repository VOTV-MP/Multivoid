# session_streams extraction — design + as-built (2026-07-18 s23)

> STATUS: BUILT (see §5 as-built evidence). The frozen-instrument recipe's third
> application (after the rack extraction `73dc9ba1`); the first one where the
> refactored surface is NONDETERMINISTIC live streams, so the equivalence package
> replaces a content digest with: literal body diff + a mutant-proven 4-peer
> routing matrix + the already-frozen drive digest for the untouched reliable
> plumbing. /qf pass: 4 rounds, "that holds" at R4 (thread:
> scratchpad qf_thread_session_streams.md).

## 1. Why

`coop/net/session.cpp` sat at 1208 LOC (soft cap 800), and every new stream
channel (v109 hand/cursor, v111 deskSim, v113 dish, v114 reel...) grew all
three of its per-channel surfaces further. The scalar stream channels are ONE
domain concept (the per-channel unreliable pose/state streams) already
precedented as extractable member-fn TUs (session_npc/worldactor/trashcarry/
voice).

## 2. The split (all bodies verbatim)

NEW `src/coop/net/session_streams.cpp` (620 LOC) owns the NINE scalar channels:
per-peer pose/prop/ragdoll/hand/deskCursor + host-single clock/deskSim/dish/reel:

| surface | old home | new home |
|---|---|---|
| 9 `Set*` publishers (GT, localMutex_) | session.cpp:106-158 | verbatim |
| 9 `TryGet*` readers (GT, remoteMutex_) | session.cpp:164-256 | verbatim |
| 9 receive-store switch cases (net thread) | HandleMessage :516-740 | `StoreStreamPacket(type, routeSlot, peerSlot, data, len, seq)` — grouped case labels in HandleMessage delegate; bodies verbatim; `return` ≡ old `return` (nothing followed the switch) |
| step-3 stream fan-out (net thread) | NetThread :948-1127 | `SendStreamsTick(now, sendInterval, nextSend&, nextClockSend&, nextDeskSimSend&, sendFails&)` — the whole `if (Connected && now>=nextSend)` block verbatim, incl. the npc/wa/tc batch stamps (their Serialize* stay in their TUs) |

session.cpp keeps: SendReliable*/typed sends, HandleMessage's dispatch shell
(parse/epoch latch/routeSlot/batch delegates/Reliable case), NetThread's loop
shell (signaling poll, RunCallbacks, receive drain, net-diag). 679 LOC.

### The wrapper-line table (the ONLY non-verbatim lines)

1. session.h: `+<chrono>`; two private decls (StoreStreamPacket / SendStreamsTick).
2. session.cpp HandleMessage: 9 grouped `case` labels + ONE call
   `StoreStreamPacket(type, routeSlot, peerSlot, data, len, seq); break;`
   (param order matches the decl; inside, stores use routeSlot / relays use
   peerSlot exactly as the old inline cases did).
3. session.cpp NetThread: prologue keeps the two cadence time_point locals,
   DROPS the two constexpr intervals (hoisted into the new TU); step 3 =
   `const auto now = steady_clock::now();` (STAYS in the shell — step-4
   net-diag shares the same single timestamp, unchanged semantics) +
   `SendStreamsTick(now, sendInterval, nextSend, nextClockSend, nextDeskSimSend, sendFails);`.
4. session_streams.cpp: the two function heads; `auto* sockets =
   SteamNetworkingSockets();` (the SendReliable* re-fetch idiom); the two
   hoisted `constexpr` cadence intervals; the `switch (type)`/`default` shell;
   section comments.

## 3. /qf round map (4 rounds, fresh critic each)

R1: instrument choice — synthetic injection via public Set* REJECTED (live-consumer
side effects unmeasured; busy channels race live producers); the wrapper-coverage
hole (a routeSlot/peerSlot swap is invisible to a 2-peer smoke) is closed by the
4-PEER smoke instead (relay exercises routeSlot!=peerSlot on clients). Signature
read-sets enumerated line-by-line. R2: the `now` seam caught (step-4 shares it)
-> `now` stays in the shell; pose-diag keying MEASURED (net_pump.cpp:1200-1216
prints per-slot fresh/trail from TryGetRemotePose(slot)). R3: sockets re-fetch
declared as wrapper; three-build hygiene (SHA x4 before EACH smoke; s_1234
restore measured unnecessary — the drive digest is save-drift-tolerant across
4+ historical runs). R4: "that holds".

## 4. Acceptance package (from the /qf pass)

1. Literal body diff (streams_body_diff.py, stripped-line compare, wrapper lines
   excluded per the table) — KNOWN-POSITIVE proven: a deliberately inverted
   newest-wins compare in a mutated copy is flagged, exit 1.
2. The wrapper-line table above, eyeballed (this doc).
3. drive_selftest digest circle (frozen values cea1940d5997c1f3 /
   c4b0a7012829f902 / 2f57cabc0b11d213) on the 2-peer smoke.
4. 4-peer smoke matrix (mp.py smoke4's per-client CROSS-PEER verdict) —
   KNOWN-POSITIVE proven by a MUTANT build (routeSlot/peerSlot swapped at the
   wrapper call) that must FAIL the matrix before the real build's clean PASS
   counts.
5. Negative grep of moved symbols in session.cpp (own known-positive: the same
   pattern hits in session_streams.cpp).
6. Reconnect surface: rides the 2p+4p connect/teardown in the smokes.

## 5. As-built evidence (2026-07-18 late night)

- BASELINE (pre-extraction DLL `32aac4e57168d44d` x4):
  - 2p 120s: PASS; host circle inject c4b0a7012829f902 -> remove cea1940d5997c1f3;
    client circle inject 2f57cabc0b11d213 -> remove cea1940d...; host cross-ticked
    the client row 1x. (Host->client cross structurally absent: the host circle
    completes before the joiner's rack mirror resolves — timing artifact, must
    reproduce in B.) scratchpad/streams_baseline_2p.txt.
  - 4p: PASS — every client "CROSS-PEER OK -- sees host + [both peers] via relay",
    stale_drops=0 malformed_drops=0 puppet_fail=[]. scratchpad/streams_baseline_4p.txt.
- LITERAL DIFF: accessors 130 / cases 224 / step3 178 stripped lines identical;
  known-positive exit 1 on the mutated compare.
- NEGATIVE GREP: no moved definitions remain in session.cpp (comment mentions only).
- MUTANT 4p (DLL `46ef33cd76047636` x4, routeSlot/peerSlot swapped at the wrapper
  call): **FAIL exactly as required** -- all three clients "saw 0 cross-peer
  puppet(s), expected 2 (RELAY GAP)", puppet_slots=[0] only. The matrix
  instrument is PROVEN discriminating. scratchpad/streams_mutant_4p.txt.
- EXTRACTION (DLL `667b49c26e11da07` x4, hash-verified before each run):
  - 2p 120s: PASS; digest pattern IDENTICAL to baseline -- host circle
    c4b0a7012829f902 -> cea1940d..., client circle 2f57cabc0b11d213 ->
    cea1940d..., client->host cross-tick present (2x), host->client cross
    absent (same structural timing as baseline). Zero net/stream WARNs, zero
    stale-gen drops, pose-diag fresh=61/s trail=0cm, netPumpTick 0.20-0.25
    ms/fr. (A first 2p attempt aborted on a host boot anomaly with no verdict
    -- discarded, rerun clean.) scratchpad/streams_extraction_2p.txt.
  - 4p: PASS -- verdict IDENTICAL to baseline: every client CROSS-PEER OK
    (sees host + both peers via relay), stale_drops=0 malformed_drops=0
    puppet_fail=[]. scratchpad/streams_extraction_4p.txt.
- WRAPPER AUDIT (agent, full-file): PASS on all 8 checks -- param binding incl.
  the same-typed time_point& pair, the single `now`, constexpr relocation
  values, lock discipline, return/break equivalence (nothing follows either
  switch), GNS-free header, no behavior additions, sizes confirmed.
- File sizes: session.cpp 679, session_streams.cpp 620 (both < 800).
- Honest gap: a same-process disconnect+reconnect cycle was not separately
  driven (each smoke exercises one full connect/teardown; the moved code holds
  per-process stream state only, reset by ResetPeerRemoteState which did NOT
  move). NOT hands-on.
