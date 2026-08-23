# Signal/email ready-edge seeds — design of record (2026-08-23, `/qf` 9 rounds, "that holds" at R9)

## AS-BUILT (2026-08-23 eve, same session — read this box first)

**BUILT + drilled the same evening.** Final DLL `2DF5783998242373` deployed ×4 (the RED/GREEN
runs below ran on it; the first RED ran on the pre-timing-fix `3475A9A4CDDB5B23`), proto 134
unchanged (zero wire change as designed), **NOT hands-on**. As designed except where noted:

- **Owner:** `coop/session/join_seed.{h,cpp}` (90+111 LOC) — Seeder class + LaneAdapter fn-ptr
  struct + the engine-free delta selftest. Both lanes instantiate it (`g_seeder` + three adapter
  fns each); public `CaptureJoinSnapshot`/`CancelJoinSnapshot`/`QueueConnectBroadcastForSlot`/
  `OnDisconnectSlot` per lane, hooked beside meadow's at `save_transfer.cpp` (capture+cancel) and
  `subsystems.cpp` (seed in ConnectReplayForSlot; per-slot teardown in DisconnectSlot).
- **Commit-0's code read OVERTURNED the R6/R7 vacuous-success assumption** (§2.6 rewritten):
  `SendReliable` returns `anySuccess` = FALSE with zero eligible receivers — the solo-author
  retry dup was LIVE (deliberate audit-I-1 hold-and-retry). Scoped fix shipped: both lanes adopt
  `sent=true` on a refusal with `!AnyWorldReadyPeer()`; the global-bool ambiguity + its ~15
  consumer lanes censused and FILED (§4).
- **signal_sync also gained the `world_load_episode` Tick gate** (the email 2026-06-19
  false-delete class, measured latent in signal — same positional diff, no gate).
- The apply parks are FIFO-once-nonempty deques (cap 256 → `Session::SetApplyBackpressure` →
  the D10 inbox pause; `kMaxApplyRetries=30` with the engine RESOLVED = loud drop, the
  malformed-blob class). The relay-eligible flag is `relayEligible_[slot]` (atomic hConn stamp,
  set at CWR net-receipt in the receive path, read at `session_relay.cpp`'s gate, cleared beside
  all three `backlog_.FreeSlot` sites). `Assembler::ClearSlot` shipped + wired for both lanes.
- **Drill evidence (all autonomous):** selftest **6/6 PASS** (gap-append/gap-deletion/multiset
  ×2/net-zero/both-signs). **RED** (mutate `VOTVCOOP_SEED_DISABLE=1`, drill authoring): snapshot
  16:09:30 → in-window email authored :38 (8 s POST-snapshot, not in the save) → client wire
  applies **0** — the loss class reproduced. **GREEN** (seeds on): capture "1 distinct hashes"
  (the solo email = the baseline) → in-window authored 9 s post-snapshot → ready edge
  `email_sync: seed slot=1 +1/-0 rows` → client applied **exactly one** wire email, topic
  `[seed-drill] in-window`; the solo email exactly-once (0 wire applies — the dup fix measured).
  signal seed wiring live (capture + no-op seed on the empty signal array). Smoke PASS, 0 ERROR
  both peers. Drill caveat: the first drill run authored at 3 s and RACED AHEAD of the snapshot
  (email rode the save — the loss case unexercised); the drill now waits 12 s and the log ORDER
  (authored-after-captured) is the per-run proof. One unrelated client boot-death flake observed
  once (died 4 s into boot, pre-dispatch, no dump; not reproduced).
- Files over the soft cap after this arc: `session.h` 895 (+40) and `save_transfer.cpp` 1014
  (+8) — both carry standing extraction proposals (session.h split; the HostStream pump TU).

**The ask (user):** the queued arc after R-4b — emails/saved signals authored while a friend joins
vanish for the joiner, forever. Green-lit via "pc free, go" on the stated queue.

**Thread transcript:** session scratchpad `qf_thread.md` (pass 3). This doc is the converged shape +
the commit-0 measurements; an AS-BUILT box lands on top after the build.

## 0. The gap, measured

`SendReliable`/relay skip `!IsSlotWorldReady` slots with no queue (B2, BY DESIGN — R-4b round 1
boundary decision: queueing pre-world lines would dupe the connect replay). Both lanes' headers
claim "a joiner starts converged via the v56 save transfer; live deltas only" — TRUE at the
snapshot instant, FALSE for the 30-60 s load window. Neither lane had any snapshot hook, any
ConnectReplayForSlot entry, or any world-ready gate. The proven idiom is meadow_db_sync's seed
pair (v120): capture at `save_transfer.cpp:552` (OnRequest, the same GT callback that serializes
what the joiner's save will contain), seedDelta at the ready edge, `CancelJoinSnapshot` at `:627`.

## 1. The converged design

1. **Shared seed helper** (`coop/session/join_seed.{h,cpp}`): per-slot content-hash MULTISET
   snapshot (capture reads the ARRAYS directly; unreadable ⇒ fail the whole capture, warn —
   meadow `:763-766`); at the ready edge seedDelta(h) = cur(h) − snap(h) over the union: d>0 sends
   d append copies, d<0 sends −d hash-keyed deletes (meadow `:827-833` ported). Consume-once
   (snap invalidated), `seededOnce` warn-once (cave-travel re-fires are normal), Cancel on
   teardown. Per-slot arrays ⇒ concurrent joiners independent by construction. **No
   unmaskedPendingNet term**: the twins have no pending-retry structure (meadow's mask models its
   own); see §2 fact 6. Meadow does NOT migrate (its order channel + pending-mask are the
   measured non-fitting seams; RULE 2 does not demand rewriting a field-proven instance).
2. **Client send gate** in both lanes: `IsHost() || net_pump::HasAnnouncedWorldReady()` (the
   meadow_db_sync.cpp:123 gate both lanes lacked) — a joining client's pre-ready authored rows
   would otherwise ride the seed back as dups.
3. **Receive-side park-until-appliable** replacing the measured warn-and-drop
   (`email_sync.cpp:217-220` "engine unresolved — row lost", `:231` "apply FAILED — row lost",
   + the signal twin): a FIFO-ONCE-NONEMPTY apply queue (the send_backlog discipline mirrored
   receiver-side — while non-empty, new arrivals append behind it; drain applies in arrival
   order). Event-anchored on own world-up (never a wall-clock TTL —
   [[lesson-wall-clock-ttl-on-cross-lane-dependency-is-a-drop]]); parks survive world-down within
   a session (their purpose), are emptied at the lanes' OnDisconnect (the existing DisconnectAll
   teardown home). Bound 256 (flood backstop, picked with ~2 orders headroom over the lanes'
   handful/minute authoring, non-load-bearing): overflow flips the D10-style inbox pause
   (pause-not-drop; GNS receive lossless-by-stall) — terminal honesty is the HOST's existing
   per-connection no-progress fatal (session.cpp:660 per-slot CheckFatal → FatalCloseSlot(i)).
4. **Net-thread relay-eligible flag, per-slot + hConn-STAMPED** (the send_backlog anti-recycle
   idiom), set at the net-thread RECEIPT of ClientWorldReady, read by `session_relay.cpp:86`'s
   skip check, cleared at the FreeSlot sites. Closes the found micro-window (§2 fact 3) for ALL
   seeded lanes including meadow. A recycled slot cannot inherit eligibility (hConn differs).
5. **Per-slot `Assembler::ClearSlot`** called from the slot-transition fan-out
   (`subsystems.cpp:350` DisconnectSlot) for the two lanes — closes the recycle-merge for them;
   the tree-wide Assembler-owners census is FILED (§4).
6. **Explicit Normal lane pins** for RosterRow + EmailChunk/EmailDelete/SavedSignalAppend/
   SavedSignalDelete (today all default-Normal; pinned so a future lane move can't split the
   FIFO ordering proof — the meadow precedent).
7. **Zero wire change** — existing kinds carry the seed; no proto bump.

## 2. The exactly-once construction (why no double, no loss)

1. **The two legs partition time at the ready edge.** Capture at the connect-side OnRequest; the
   seed's `cur` is read at the READY edge inside the ClientWorldReady GT drain case —
   `event_feed.cpp:230` (MarkSlotWorldReady) then `:231` (ConnectReplayForSlot), one synchronous
   callstack, so NO GT-authored broadcast can interleave between flip and seed. Rows authored in
   the gap are in cur, out of snap ⇒ the SEED leg; the live leg is CLOSED for the whole gap (the
   four kinds are NOT in `IsPreWorldSendableKind` — measured).
2. **The net-thread edge is exactly-once by the stamped flag** (four-interleaving proof, R2):
   received-before-CWR rows drain before CWR in the single FIFO inbox ⇒ in cur/seed, never
   relayed; received-after rows relay directly ⇒ applied to the host array post-cur-read ⇒ never
   seeded.
3. **The found micro-window (pre-existing, meadow shared it since v120):** a peer row received on
   the net thread in [CWR receipt, GT flip] was relay-skipped AND applied post-seed ⇒ lost. The
   flag closes it. Width was ms (GT drain latency).
4. **Ordering through backpressure is FIFO-exact, not causal luck:** roster row + chunks ride the
   same (slot, lane) stream; the send-backlog preserves per-(slot,lane) FIFO through episodes.
   Causality (join-to-world-ready ≥30 s) is defense-in-depth only.
5. **Delete races:** receiver state is the native array (no counts to underflow); a no-match
   delete TOMBSTONES (existing, 20 s TTL) and kills a late append ("delete won the race") or
   expires silently. In-window author+delete nets to delta 0 (in neither leg). Order divergence
   across peers is PRE-ACCEPTED in these lanes since v65 (email_sync.h:29-31 — hash-keyed deletes
   exist BECAUSE of it); meadow's order channel was a meadow-specific user decision. If the user
   wants email/signal-pane order synced: separate product ask (residual).
6. **The retry cannot cross the ready edge — after commit-0's fix.** Measured: the lanes' per-row
   `sent=false` retry fires on the first CONNECTED poll (1 Hz), ≥30 s before any ready edge, and
   nothing is ever addressed to a not-ready slot. **BUT the commit-0 code read overturned the
   vacuous-success assumption: `SendReliable` returns `anySuccess` = FALSE with zero eligible
   receivers (session.cpp:241-250) ⇒ a row authored while NO other peer was world-ready retries
   until someone readies ⇒ delivered to a joiner whose save already contains it ⇒ DUPLICATE.
   This dup bug is LIVE TODAY (pre-existing, since v64/v65), the mirror image of the loss this
   arc fixes.** Scoped root fix: the seeded lanes adopt `sent=true` when no peer is world-ready
   (vacuous success — absentees are owed save+seed, valid precisely BECAUSE the seed now exists).
   The global bool stays (a census found ~15 lanes consuming it as "did it go out"; un-seeded
   lanes lean on retry-until-heard as their de-facto late-join path — flipping them without seeds
   trades dup for loss). The ambiguity itself + per-lane exposure: FILED (§4).

## 3. Verification plan

- 2-peer **RED** (pre-build bytes): host authors an email + saves a signal during the client's
  load window (host-settle + scripted authoring) ⇒ client array diverges (count/hash mismatch).
  **GREEN** (built bytes): converges. **Mutate control**: capture disabled ⇒ RED returns.
- Deterministic selftests (in-process, frozen-instrument): delete-race order A (delete-unknown ⇒
  tombstone ⇒ append dropped) and B (append ⇒ delete removes one instance); GAP-DELETION delta
  math (in snap, absent cur ⇒ one delete); multiset counts (two identical rows ⇒ d=2).
- The author-solo → join → exactly-one-copy case rides the RED/GREEN drill (needs two processes).
- The 4-peer variant is UNJUDGEABLE on the cross-peer axis until the pre-existing relay-gap row
  (§4) is fixed.

## 4. Filed (not this arc's to fix)

- **4-peer relay gap**: baseline smoke4 FAILs today — earlier joiners never see the LAST joiner's
  puppet (identical with/without rate knobs; measured 2026-08-23). Pre-existing; own row.
- **Tree-wide Assembler per-slot teardown census**: every blob_chunks::Assembler owner
  (laptop_buffer, laptop, container_contents, meadow, comp, drive, player_inventory, chat…) shares
  the recycle-merge exposure §1.5 closes for the twins.
- **The ambiguous fan-out bool**: SendReliable's false conflates "refused" with "zero eligible";
  each retry-on-false lane needs the seeded-lane treatment or a mask when it gets its own seed.
- Email/signal-pane ORDER sync: separate product ask if ever wanted.
