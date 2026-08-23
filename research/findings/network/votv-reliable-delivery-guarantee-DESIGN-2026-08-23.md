# The reliable delivery guarantee — R-4b / b125 §R-A design of record (2026-08-23)

## AS-BUILT (2026-08-23, same day — read this box first)

**BUILT in 4 commits from `a39a19cd`** (commit-0 knobs → the backlog core → D4+D9+D10), proto 134
unchanged (no wire-shape change), DLL deployed ×4, **NOT hands-on**. Owner:
`coop/net/send_backlog.{h,cpp}` (121+205 LOC). Deviations from the plan below, all documented:

- **The drain runs on the NET THREAD** (NetThread step 3b), not net_pump's game-thread tick — the
  backlog is engine-free and the net thread survives game-thread stalls. Same locks either way.
- **The mutex is per-SLOT (covering its 3 lanes), not per-(slot,lane)** — the FIFO-once-nonempty
  proof only needs atomicity per lane; one mutex per slot is the same correctness with less state.
- **`SendRateMin/Max` was NOT raised** — and the "GNS default is a fixed 256 KB/s clamp" fact this
  bullet originally recorded was **FALSE for our binary** (pass-2 correction, 2026-08-23 §7): it
  measured GNS *stock* defaults, but `session_start.cpp:79-84` has set global Min=1 MB/s /
  Max=25 MB/s since 2026-06-06 (`8dd62916`), and the field host's net-diag reads exactly
  1,048,576 B/s in 180/180 samples. The rate DECISION is §7; `net.sendrate_kbs` pins for drills.
- **`ArmBeginNoSave_` unifies ALL Begin sends into the pump** (the no-save announce is a
  zero-chunk stream) — the design only demanded the blob path's Begin be gated.
- **D10's client pause threshold is a SOFT cap (6144)** below the 8192 hard cap, so the client
  can never reach the dropping branch at all.

Drill evidence: **RED** (commit-0 bytes, 128 KB pin) = 956 silent losses in one LAN join
(GrimeState ×632 / TrashPileState ×306 / ContainerContents ×18 — the class was never
PropSpawn-specific) + 267 parks; host "sent" counter lying (3127/3128). **GREEN** (same pin) =
rc=-25 **0**, episodes "all delivered", 285 parks 0 expired. **Throttled** (128 KB buffer +
256 KB/s rate, the 19.4 MB test save) = ONE 5 s episode, 5,783 msgs peak 983 KB all delivered,
**poses flowing mid-episode** (puppet spawned the same second the episode opened — D8 proven),
claim sweep verdict **0 unclaimed destroyed** (the §4-fenced sweep-shrink inference now measured).
**Unpinned baseline** = episodes 0 (D4's 4 MB absorbs the burst), D9 re-stamp line live, logs
clean. Note for future throttle drills: the test save is **19.4 MB** (339 chunks) — a rate pin
below ~256 KB/s starves the transfer past the smoke's join window.

**Status: DESIGN converged (`/qf` 6 rounds, critic "that holds" at R6) — then BUILT same day (see
the AS-BUILT box).**
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
- **Shape (b) of §R-A (pre-world-gate sharers): BUILT the next evening (`0676e5a8`)** —
  `signal_sync` + `email_sync` got ready-edge seeds via the shared `coop/session/join_seed`
  helper (NOT queue absorption — queueing gate-skips would dupe the replay, the boundary this
  section decided). Design: `votv-signal-email-ready-seeds-DESIGN-2026-08-23.md`.
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

## 7. The SendRate decision (pass 2, 2026-08-23 — `/qf` 5 rounds, converged)

**User delegated: "SendRate decision - I'm not sure, go with what's best."** Round-1's demand to
MEASURE instead of carry reframed the whole question — the §AS-BUILT premise was false.

### 7.1 Corrected fact base (all measured)

- `session_start.cpp:79-84` sets **global SendRateMin = 1 MB/s, SendRateMax = 25 MB/s** at GNS
  init — shipped in `8dd62916` (2026-06-06, proto v40 era). The R-4b pass measured GNS *stock*
  defaults (`csteamnetworkingsockets.cpp:84-85` = 256 KB/s) and recorded them as our binary's —
  without censusing our own init path for overrides. The June memory
  (`project_puppet_lag_interp_starvation.md:162`) had recorded the raise correctly all along.
- **Effective-rate mechanism** (vendored `snp.cpp`): rate = clamp(init-estimate, Min, Max);
  the estimate (`m_nCurrentSendRateEstimate`) is written ONLY at `SNP_InitializeConnection:286`
  (4380 B / ping-at-init) and by the clamp itself (:4253/:4263/:4268). **No loss-, ack- or
  timer-driven writer exists** ("FIXME — we might implement BBR probe cycle"). Any internet ping
  > ~4.4 ms ⇒ rate sits at **Min = 1 MB/s forever**; LAN sub-ms ping ⇒ Max. `session_start`'s
  old comment "GNS still adapts the rate DOWN toward Min on a lossy link" was FALSE (fixed).
- **Field artifacts match the model exactly.** The Linux pair's HOST log: net-diag
  `sendRate=1048576B/s` in **180/180 samples** (ping 19-25 ms, including the 485-drop burst
  second — adaptation would have moved it). Her 18 MB save crossed in ~18 s (:31336/:32678 client
  log) = 1.0 MB/s. Our unpinned loopback smokes run 3.9-10 MB/s under the 25 MB/s ceiling
  (pump-paced). The 19 SEND BACKLOG warns in her log = exactly the 18 s transfer window (intended
  buffer-full pacing) + the one rc=-25 burst second (the R-4b class, fixed). **Zero chronic
  backlog** — her link absorbed the 1 MB/s floor cleanly.

### 7.2 Decision of record

**A. NO rate-value change.** The residual question ("raise the 256 KB/s default?") was moot — the
raise shipped 2.5 months ago. Min=1 MB/s / Max=25 MB/s stands: field-measured clean at Min on the
internet path; knob `net.sendrate_kbs` overrides per-connection in both directions. Raising Min
further buys ~9 s per join while overdriving more uplinks with zero adaptation — rejected.
**The thin-uplink case (host uplink < 8 Mbit ⇒ fixed-rate overdrive) is OPEN-UNMEASURED**, not
"unmotivated": absence of reports from a small tester base is not a measurement. It is
pre-existing shipped behavior; §7.3 is its instrument. `session_start`'s "[P2P-over-slow-internet
follow-up: topology-aware Min]" note stays open and points here.

**B. Premise-contamination census (7 sites, all fixed 2026-08-23):** this doc's AS-BUILT bullet;
`docs/LESSONS.md` R-4b lesson row; `session_start.cpp:76` (false adapts-DOWN comment);
`session_status.cpp:71-72` (stock-vs-ours ambiguity); memory `MEMORY.md` НОВЫЙ-ФАКТ line; memory
r4b topic (description + fact + OPEN item); memory enqueue-loss lesson. **Constants re-audit:
no shipped R-4b constant derived from the false premise** — kNoProgress=30s is drain-motion-based,
kMaxBytesPerSlot=16MB is 20× the measured 742 KB burst, kReserve=64KB is snp.cpp:320 arithmetic,
4 MB buffer is burst-vs-buffer sizing, save pump `kChunksPerTick=4` is backpressure-paced by
explicit comment. Prose only. New lesson: `[[lesson-a-librarys-default-is-not-your-binarys-config]]`.

### 7.3 Drill: overdrive survivability (RUN + PASSED 2026-08-23 eve — results in the status box)

> **STATUS: RUN 2026-08-23 eve (PC freed) — ALL FOUR RUNS PASS THE PRE-REGISTERED KEYS; overdrive
> survivability is MEASURED.** DLL `8DF99D90D747081E` ×4 (the fakelink knob build). Results:
> - **Run 1 CONTROL** (rate 256 = fakelink 256): PASS — transfer 19.3 MB in 77 s = exactly
>   clamp-paced, crc ok, rc=-25 0, 0 ERROR, both knob log lines live.
> - **Run 2 OVERDRIVE** (rate 1024, fakelink 256, 4 MB buffer): **PASS — the decision-A branch.**
>   Transfer capacity-bound 79 s (same wall-clock as control); GNS send buffer sat pinned ~4.1 MB
>   for 83 s (SEND BACKLOG warns = intended pacing); net-diag `qual=100/26%` (the ~75% wire loss
>   is real and ARQ recovered all of it); crc ok, 0 rc=-25, 0 ERROR, no fatal, no kick, poses/
>   puppet fine. **Sustained 4× overdrive degrades gracefully and never dies** — the thin-uplink
>   case does NOT reopen decision A.
> - **Run 3 CONTRACT-PATH** (run 2 + 128 KB sendbuf): PASS — the backlog engaged under overdrive
>   and closed three episodes "all delivered" (largest 5,544 msgs, peak 930 KB), transfer 79 s,
>   crc ok, 0 errors, no fatal.
> - **Run 4 AGGREGATE** (4-peer smoke4, fakelink 768): the DELIVERY axis passed (all 3 saves
>   crc-ok, episodes closed, 0 rc=-25, no fatal) — but the smoke's cross-peer verdict FAILED on a
>   relay gap (earlier joiners never see the LAST joiner's puppet) that reproduces IDENTICALLY in
>   an unpinned BASELINE smoke4 → **pre-existing, independent of rate/overdrive; filed as its own
>   row** (see the b125 R-F family / new row below). The aggregate cross-peer axis is unjudgeable
>   until that row is fixed; smoke4's joins are also serialized (one at a time), so the
>   simultaneous-3-join worst case remains a modeled, not measured, corner.
> Logs: session scratchpad `drill/run{1..4}_*.log`.

Instrument: new registry testing-knob row `net.fakelink_kbs` (same shape as the two R-4b knobs;
one init-time `ResolveInt` read in `session_start` ⇒ `SetGlobalConfigValueInt32(FakeRateLimit_Send_Rate)`.
Gate-clean: `registry_gate.ps1` counts any `rows::` token; no exemption). FakeRateLimit is a
**policer** — it silently DISCARDS packets beyond the token bucket (`lowlevel.cpp:1885-1907`) —
i.e. worst-case thin-uplink physics (no queueing, pure loss).

Topology: **host + client_1 only** (the policer is process-global and starves acks/keepalives of
every host connection — no bystander peer may confound the verdict). Client→host is unthrottled.

Runs (host-side knobs; 19.4 MB test save):
1. **CONTROL**: `net.sendrate_kbs=256` + `net.fakelink_kbs=256` (zero overdrive) — must PASS
   clean; isolates overdrive as the only variable in run 2.
2. **OVERDRIVE**: `net.sendrate_kbs=1024` + `net.fakelink_kbs=256` (the 4× thin-uplink case,
   default 4 MB buffer) — the product-realism run.
3. **CONTRACT-PATH**: run 2 + `net.sendbuf_kb=128` (the R-4b RED recipe) — forces the backlog to
   engage under overdrive (with the 4 MB buffer it rarely does).

Verdict (three-way, pre-registered):
- **PASS** = transfer completes capacity-bound (~78 s at effective ~256 KB/s), any backlog
  episodes close "all delivered", claim sweep 0 unclaimed destroyed, 0 ERROR, save written crc-ok.
- **DESIGNED-FATAL** = a `FatalCloseSlot`/kick line with reason — contract-legal ("or the
  connection dies") but a **PRODUCT FINDING that REOPENS decision A** (thin-uplink joins would die
  rather than degrade; motivates lowering Min or topology-aware Min).
- **FAIL** = neither key within the smoke's bounded window (a silent stall times out as FAIL,
  never hangs unjudged). Attribution discriminators: GNS `qual=`/pending in net-diag + save-chunk
  progress cadence (transport physics) vs a backlog episode stuck WITH buffer space free
  (contract defect).

**Aggregate threat model (pre-registered):** Min is per-CONNECTION — a host serving 3 clients
floors at **3 MB/s aggregate** (24 Mbit) during simultaneous bursts. A clean 2-peer PASS
therefore does NOT clear the 4-peer field case; after runs 1-3, a fourth confirmation run
(host + 3 clients, `net.fakelink_kbs=768`) is the field-shaped variant.
