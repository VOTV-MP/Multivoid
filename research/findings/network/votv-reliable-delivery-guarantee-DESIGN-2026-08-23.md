# The reliable delivery guarantee — R-4b / b125 §R-A design of record (2026-08-23)

**Status: DESIGN, converged (`/qf` 6 rounds, critic "that holds" at R6). NOT BUILT.**
Thread transcript: session scratchpad `qf_thread.md` (2026-08-23). Supersedes the fix-direction
prose in `votv-tester-log-triage-b125-2026-07-26.md` §R-A and
`votv-linux-fps-triage-2026-08-23.md` §5 for R-4b; those rows point here when built.

## 0. What broke, in one paragraph

GNS "reliable" is ARQ only for messages that ENTER the stream. Under send-buffer backpressure
(`PendingBytesTotal() + cbData > SendBufferSize`, default 512 KB —
`steamnetworkingsockets_snp.cpp:320`, `csteamnetworkingsockets.cpp:78`) the send is refused with
`-k_EResultLimitExceeded` (-25) and `bDeleteFailedMessages=true` deletes the message
(`session.cpp:177`). ~60 call sites ignore the false return. Field (Linux triage, 2026-08-23):
485 PropSpawn refusals in one join minute → host snapshot applied 2,607/3,093 → 957/1,829 client
locals unclaimed → the >50% ratio valve CORRECTLY refused the divergence sweep → 284
`container_contents` parked, **282 expired = containers permanently empty on the joiner**. The
join burst is STRUCTURAL: 3,093 × ~240 B on-wire ≈ 742 KB > 512 KB
(`PropSpawnPayload` = 212 B, `protocol.h:3310`).

## 1. The class: SIX silent-loss surfaces (all measured this pass)

| # | Surface | Site | Behavior today |
|---|---|---|---|
| 1 | `SendReliableToSlot` enqueue refusal | `session.cpp:177-185` | warn (count-only) + delete |
| 2 | `SendReliable` fan-out refusal | `session.cpp:230+` (same TU) | warn + delete, per slot |
| 3 | Host relay fan-out refusal | `session_relay.cpp:102-105` | **no warn at all** + delete |
| 4 | Pre-world gate skip | `session.cpp:145` | silent false — BY DESIGN (connect replay reconstructs); NOT a loss for replay-covered kinds; un-covered sharers = §8 |
| 5 | Receiver inbox cap | `session.cpp:456-458`, `kReliableInboxCap=8192` | warn + DROP |
| 6 | Park TTL expiry | `container_contents_sync.cpp:668-671`, 30 s wall-clock | warn + DROP |

GNS BELOW the app is lossless-by-stall on receive (measured): reassembly overflow does not ACK
(`snp.cpp:3479-3493`); decoded-queue overflow (`RecvBufferMessages=1000`,
`connections.cpp:2672-2676`) propagates as "Don't ack this packet!" with no stream advance
(`snp.cpp:3807-3816`). Loss lives only in OUR layer.

## 2. The invariant

**A reliable send either enters the stream, enters the session's pending queue, or the connection
dies. Never warn-and-drop.** Guarantee scope = the connection's lifetime: a slot's teardown frees
its queues (peer state dies with the peer — MTA `CNetServerBuffer` shape,
`reference/mtasa-blue/Server/mods/deathmatch/logic/net/CNetBuffer*` + `CNetBufferWatchDog`
verified: queue + watchdog, suspend-droppable-class, reliable never silently dropped).

New bool contract at every call site: **true = will be delivered (stream or queue); false = never
will be (pre-world gate / dead slot / len)**. Census result: both existing caller idioms
("true → latch bookkeeping", "false → re-arm later") remain correct with zero double-sends.
`blob_chunks`' abort-retry contract (`blob_chunks.cpp:43-49`) is KEPT — its false-leg stays
reachable via gate/dead-slot; only its backpressure leg goes unreachable (comment update, no
RULE-2 deletion).

## 3. Design items

- **D1 — per-(slot, lane) FIFO queues in `Session`.** Lanes are GNS's ordering domain
  (High/Normal/Bulk, `session_lanes.h:25-30`); a per-slot queue would collapse lane independence.
  Entries = FINAL wire bytes (post-`WriteHeader`), so the relay's prebuilt packets share the
  shape. Absorb ONLY `-k_EResultLimitExceeded`; the full rc set is enumerated
  (`isteamnetworkingsockets.h:260-267`) — `NoConnection`/`InvalidState` = dying connection
  (teardown owns it), `InvalidParam` = our bug (loud error). **FIFO-once-nonempty:** one small
  mutex per (slot,lane) held across [queue-empty check → `SendMessages` → on-refusal append] —
  the atomicity that holds across both producer threads (game-thread authors, net-thread relay).
  Each queue carries an **hConn stamp**: the drain discards the queue whole when
  `peerConns_[slot]` no longer matches — slot recycling (X→Y, roster doctrine) can never deliver
  X's backlog to Y even if a teardown path missed the free. Teardown frees at the
  `peerConns_[slot].store(0)` site (`session_status.cpp:315`) under the same mutex.
  Stale-stamp safety measured: header `seq` is read ONLY by unreliable stores
  (`session.cpp:393-406`); `senderEpoch` = one central exact-match latch (`session.cpp:330-334`)
  stable across queue delay.
- **D2 — the save family is the pump's try-send lane.** `SaveTransferBegin` + `SaveTransferChunk`
  bypass the queue; the pump already success-gates chunks (`save_transfer.cpp:576-579`) and
  **gains success-gating on Begin** (today BOTH Begin sends ignore the return —
  `save_transfer.cpp:175, :200-202` — a live bug this fixes: retry Begin next tick until
  accepted, only then chunks). Safe because pre-world the pump is the SOLE writer on the
  host→joiner (slot,Bulk) lane (gate `session.cpp:145`; allowlist∩Bulk = the save family +
  the opposite-direction Request). `SaveTransferRequest` rides the queue (guaranteed).
- **D3 — bound policy: connection-fatal, never drop.** Two exits, both = kick + feed line:
  (a) progress-based — queue non-empty AND zero departures for N s (a slow-but-DRAINING link is
  never kicked; a dead link dies at GNS's own connected-timeout first); (b) a generous per-slot
  byte ceiling (order 8–32 MB, ~10× worst measured need; join-burst peak ≈ 742 KB + replay −
  buffer ≈ ~1 MB). No auto-rejoin exists (disconnect → menu → manual), so no kick-storm.
- **D4 — raise `k_ESteamNetworkingConfig_SendBufferSize`** per connection to 2–4 MB (config
  range allows 4 KB–256 MB). The queue becomes the correctness net, not the common path.
- **D5 — the relay path (`session_relay.cpp`) feeds the same queues.** Its refusal today has NO
  warn — the worst surface; the field's 485 count UNDERCOUNTS for >2-peer sessions.
- **D6 — instrument:** per-(slot,lane) queue depth in net-diag + one-line open/close episode
  fold. The b125 "log eid+key on rejection" instrument is moot (nothing is dropped).
- **D7 — RED-then-GREEN drill (the acceptance IS the reported symptom).**
  **Commit-0 precedes everything:** the pin knobs (SendBufferSize / SendRateMax config rows) +
  the stats lines (applied count, park stats, sweep stats) — read-only, zero behavior change.
  RED on commit-0's build: pin buffer ~64 KB → the rc=-25 losses + expired parks MUST reproduce
  locally (an instrument that cannot go RED proves nothing). GREEN on final bytes, identical
  instrument: queue episodes logged, **host-sent == client-applied == candidate count, 0 expired
  container parks, 0 rc warns**, sweep stats recorded (candidates/claimed/doomed/abort-or-not).
  PLUS a **throttled-link run** (`k_ESteamNetworkingConfig_SendRateMax` pinned ~128 KB/s) so
  acceptance is measured at realistic drain TIMING: 0 expired parks AND live poses throughout
  (D8's proof). Same save-pair as the R-4a repro; both send paths exercised (multi-client).
- **D8 — drain headroom reserve.** The pump refills a connection only while
  `pendRel + pendUnrel < SendBufferSize − R` (fresh `GetQuickConnectionStatus` per slot per
  drain tick, episodes only; both fields already read at `session.cpp:670-675` — the sum IS
  `snp.cpp:320`'s check). Without R, a drain episode pegs the buffer and every
  `UnreliableNoDelay` pose/voice send (`session_streams.cpp:506-555`) is refused for the whole
  episode (GNS's NoDelay early-drop is an unimplemented FIXME, `snp.cpp:334-337`) = frozen
  remote players. R = 64 KB, derived: worst concurrent unreliable ≈ 3 peers × 228 B (voice
  packet, `protocol.h:3101`) × 60 Hz + poses ≈ ~75 KB/s ≈ 1.2 KB per 16 ms tick → ~30× margin.
  The rc stays the correctness backstop; R is a fairness reserve — NOT round-1's rejected
  stale-diag gate. **D1+D8 IS b125's "pace the drain against the send-buffer budget", at the
  session layer; `prop_snapshot`'s 100/tick CPU pacing is untouched.**
- **D9 — event-anchored park aging.** Parks don't age while our own join snapshot is in flight
  (Begin seen, Complete not); aging starts at SnapshotComplete — guaranteed and lane-ordered
  AFTER every PropSpawn it brackets, so a park unresolved then is genuinely orphaned; 30 s grace
  from that point; TTL survives only as leak-guard. Bracket-scoped by construction: parks die
  with the world (`container_contents_sync.cpp:869`), one Begin..Complete per join. Root reason:
  ContainerContents rides Normal, PropSpawn rides Bulk — lanes are independent, so under
  backpressure contents SYSTEMATICALLY precede their spawns; wall-clock TTL (30 s) can expire
  before a slow link drains Bulk, reproducing the symptom with zero wire loss.
- **D10 — client inbox backpressure, not drop.** On the client (per-connection receive,
  `session.cpp:579`) an at-cap inbox pauses polling instead of dropping; GNS below stalls
  un-ACKed (measured, §1) → sender retransmits → true end-to-end backpressure. Pause window is
  bounded by the game-thread stall that caused it (8192 × 240 B ≈ 2 MB ≈ ~10 s of no-tick at
  field link rate). The host's poll-group path keeps the cap as flood defense (DoS posture
  unchanged).

## 4. What this does NOT change

- **No wire-shape change; no `kProtocolVersion` bump owed by the design** (verified per-item —
  all endpoint-local; the release bumps the build number by standing policy anyway).
- **Shape (b) of §R-A (pre-world-gate sharers) is a SEPARATE arc:** `signal_sync` + `email_sync`
  have no `ConnectReplayForSlot` entry and no in-file world-ready hook (measured at HEAD) — they
  get ready-edge seeds (the B2 idiom), NOT queue absorption (queueing gate-skips would dupe the
  replay). Filed, not dropped.
- The 689 post-episode KEYED client destroys (triage §5-CORR) stay with the R-4a end-condition
  `/qf`, unattributed here.
- `prop_snapshot`, the ratio valve, and the membership sweep are untouched. The sweep-shrink
  expectation (complete snapshot → near-zero doom set) is INFERENCE fenced by D7's recorded
  sweep stats; a large doom set despite complete delivery becomes its own row.

## 5. Commit plan

0. commit-0: knobs + instrumentation (read-only). RED runs here.
1. D1+D5+D6: the queue TU (`coop/net/` — session-owned, principle-7 clean), both send paths +
   relay wired, teardown free, hConn stamp, episode instrument.
2. D2: save-pump Begin success-gating (+ the try-send API split).
3. D3+D4+D8: bounds, buffer raise, reserve.
4. D9: park aging re-anchor. 5. D10: client inbox pause. 6. GREEN + throttled-link drills;
   docs corrections in the same series: `docs/LESSONS.md:3513-3525` row, the memory lesson file,
   triage §8 R-4b row, b125 §R-A pointer, `docs/COOP_SYNC_MAP.md` owner row ("reliable delivery
   guarantee: `coop/net/session`; save family exempted to its pump"). No new `docs/COOP_*` file.

## 6. Build-phase measurables (named residuals, none block the design)

Exact R / cap / N constants (drill-tuned); the sweep-shrink outcome; whether a D10 pause ever
brushes the host's connected-timeout; per-site sweep of the ~60 callers against the new bool
contract (semantic argument says all safe; the build re-reads each).
