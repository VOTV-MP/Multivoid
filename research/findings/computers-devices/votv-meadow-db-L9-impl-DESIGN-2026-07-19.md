# L9 meadow DB sync — implementation design (v120)

**2026-07-19. STATUS: AS-BUILT (smoke x2 PASS + e2e selftest digest 0->1->0 proven
cross-peer, NOT hands-on). DLL `452973c707d9cb8d` x4, proto 120.**
15-round /qf "that holds" at R15 with the complete map; §11 = the BUILD OUTCOME deltas
(incl. the ORDER sub-lane added post-convergence by the user's per-rule-1 decision).
Thread: scratchpad qf_thread.md (rounds condensed there). Frame doc:
`votv-signal-chain-all-units-DESIGN-2026-07-16.md` §L9 (its "never addSignal replay" claim
CORRECTED in place 2026-07-19 — see §2). Precedent lane: v65 `signal_sync.cpp` (deck 58/59).

## 1. Scope (measured reframe)

L9 = **ONE array**: `saveSlot.savedSignals_0` @0x680 (0x70-stride signal rows) + its
presentation in the boot-persistent `ui_laptop` widget. `savedSignals_comp_0` @0x690 is the
DECK list's save mirror (`mainGamemode.saveObjects` writes it FROM `gamemode.savedSignals_0`;
boot loads it back) — host-owned saves make it free; NO lane.

Game surface census (measured, ResolvedOwner over ALL bp_reflection JSONs + the
dumped-on-demand `uicomp_signalSlot.json` — zero store refs there):
- Writers/readers of the store = `ui_laptop` {addSignal, removeSignal, sortSignal,
  genSignalList} ONLY. No edit/rename verb; no in-place element mutation exists (BP cannot
  write an element field without an array-property ref).
- addSignal callers: analogd ubergraph x2 — the deck "save to DB" button (Array_Get(deck
  row, play_selectIndex) -> gamemode.laptop.addSignal; NO Contains-dedupe -> the store is a
  MULTISET) and the physMod #5 auto-upload (gated on the analogd-local saveSignal succ —
  downstream of the BUTTON, not of gamemode.saveSignal, so the v65 receiver replay never
  triggers it: cross-lane echo excluded by trace).
- Verbs addSignal/removeSignal/sortSignal = EX_LocalVirtualFunction 0x45 (HALT-gate
  measured); ambient same-FName hits from other classes -> ctx class-check doctrine.

## 2. The apply author (measured; arch-doc claim corrected)

`ui_laptop.addSignal` full body = Array_Add(data) + Create(uicomp_signalSlot_C) + set
props + Array_Add(slots) + vb_signals.AddChild + lib.recountChildren. **ZERO id-mint
sites** — id-PRESERVING (the re-mint lives upstream in the unit-2/unit-4 chain). It keeps
the data array + the PARALLEL WIDGET ARRAY + per-child `ind` (recountChildren -> setIndex)
coherent in ONE call -> reflected replay IS the apply author (raw TArray append would
leave the widget list stale). `removeSignal(i)` same; its playSignal tail does
`laptop.snd.SetActive(FALSE,..)` = STOPS audio (never starts) — wire deletes audio-benign.
Widget lifecycle: created ONCE at mainGamemode BeginPlay (`gamemode.laptop`), persistent
per-world, screen-open irrelevant; the device's BeginPlay repoints `widget.laptop := self`.
Apply gate = both pointers live; pre-resolve = drop + W-log (world transitions only).
Blank image apply is engine-safe (BytesToImage(empty) -> null texture -> sampler default;
months of v65 field precedent on the deck screens).

## 3. Identity + diff (the core algorithm; three /qf reframes)

- v65's positional prefix walk is INVALID here (sortSignal = a move verb; the deck has
  none) — R2.
- Pointer RowKeys are INVALID here (BP Array_Insert deep-copies FStrings; sort churns
  pointers) — R3. Pointer-keyed memo caches are the same trap (allocator ABA) — R4.
- **Shadow = content-hash MULTISET {hash -> count}**, hash = ContentHash(Serialize(SD::Row))
  = dereferenced content, image STRUCTURALLY excluded on both ends (SD::Row has no image
  field). Move = no count change = structural no-op. Duplicates = counts. Max row blob =
  592 B (caps measured: 48 + (96+48+64+64)*2).
- Poll (1 Hz) pre-gate: full re-hash ONLY if (live count != sum(shadow)) OR a scoped 0x45
  mark fired (only addSignal/removeSignal FScriptNames registered; ctx class-check before
  mark-set). Steady state = one count read. Re-hash serializes into a REUSED scratch
  buffer. No caches.
- Diff: count decrement -> DELETE {contentHash}; increment -> APPEND {row sans image, via
  blob_chunks}. Wire = **MeadowDelta=112** (reliable, relay-whitelisted), proto 119->120.
- Prime = adopt-without-broadcast at first successful poll (v65 g_primed idiom). Measured
  ordering: prime precedes the ready flip by ~8 s in a real join (step1v3 logs 17:40:45 vs
  17:40:53); correctness does NOT depend on the gap (valid at gap->0: pre-prime appends
  absorbed by adopt, pre-prime deletes tombstoned then retried).

## 4. Tombstones (count model; worked-example verified)

`{hash -> outstanding-count + per-entry deadline}`, 20 s TTL, poll-retried; an incoming
APPEND consumes one outstanding entry (drop) — the v65 race cover. "Local-add-cancels"
REJECTED (worked example: symmetric delete + re-add -> A=1/B=0 divergence). NAMED
RESIDUAL: a same-content re-add within the TTL window can be consumed (v65-inherited,
narrow, convergence preserved).

## 5. JOIN (the seed — closes a PERMANENT loss window)

Measured: `SendReliable` + relay SKIP `!IsSlotWorldReady` slots (v56 B2) -> host lines in
[save-snapshot, world-ready] are NEVER delivered to a joiner; with no reconcile that was a
permanent divergence born at every mid-activity join (NOT a transient residual).

- Host captures a per-slot **multiset snapshot at OnRequest right after the scratch save
  serializes** (same-GT-callback; the g_blobKeys/SendBlobDivergenceDeletes precedent, same
  lifetime: outside HostStream, scrubbed at OnDisconnect AND OnRequest).
- At the ready flip (ONE synchronous GT callback): **seedDelta(h) = curCount(h) -
  snapCount(h) - unmaskedPendingNet(h)**; positive -> APPENDs, negative -> DELETEs, sent
  via SendReliableToSlot. Masks by GT-serial monotonic op-counter (pending born before
  snapshotAt(S) -> its effect is in the save -> masked for S). All window cases verified
  to exact 0-churn (incl. add+delete both-in-window: both retries land, net 0 under either
  order via the consume rule). The formula subsumes the earlier three prose rules.
- Client send gate: the lane broadcasts nothing until its OWN world-ready announce
  (net_pump "open our world-ready send gate" precedent) — kills the pre-ready authoring
  dup (a client line reaching the host before the flip would ride the seed back).
- Seed lines are ordinary MeadowDelta lines (same lane -> FIFO per connection,
  host-serialized -> no seed-vs-late-line reorder; tombstone consume is the second cover).

## 6. Runtime guards (dead-guard lesson)

- Persistent shadow/store count mismatch AFTER a re-hash reconcile -> W-log.
- Seed composition I-log per slot (`seed slot=S: +n/-m`).
- **[dev] meadow e2e self-test** (v115 audio-selftest precedent): host injects a synthetic
  row via reflected addSignal post-connect -> both peers log an order-independent digest
  (`meadow digest: n=X sum=Y`, wrapping-u64 sum of hash x count) -> host removes it ->
  n=0. mp.py smoke asserts the 0->1->0 transitions with matching sums (an empty-store
  0==0 discriminates nothing — R14).

## 7. NOT synced / user questions (defaults chosen, not blocking)

1. sortSignal order (presentational only — zero gameplay readers measured; the join save
   copy carries host order). Default: NOT synced; per-machine order divergence transient.
2. Blank image on wire-added rows (v65-inherited; the shared bulk-image lane stays
   queued). Default: accept residual.
3. A wire delete can stop an observer's active playback + shift selection — byte-identical
   to the native local-delete behavior. Default: native parity, no mitigation.

## 8. Flagged separate / inherited (NOT in v120)

- The v65 DECK lane shares the join-gap class (no seed) — retrofit candidate, own commit.
- v65 partial-send `anySuccess` mask (a per-peer send failure is invisible) — inherited.
- 64-bit content-hash collision — inherited v65 exposure.
- drive_sync.cpp rack extraction (1007 > 800) — queued mechanical commit.

## 9. Files (build plan)

- NEW `src/coop/interactables/meadow_db_sync.{cpp,h}` — the lane (all of §3-§6).
- NEW ue_wrap accessor for the saveSlot store (sibling of `saved_signals.h`, RowKey-free:
  resolve saveSlot + savedSignals_0 @0x680; Count/ReadRow/reflected addSignal/removeSignal
  thunks on `gamemode.laptop`).
- `protocol.h`: MeadowDelta=112 + payload comment; kProtocolVersion=120.
- `event_dispatch_signal.cpp`: router case (signal family, BlobChunkPayload -> assembler).
- `session_lanes.h`: lane pin + relay whitelist row.
- `save_transfer.cpp`: the per-slot snapshot capture at OnRequest (next to g_blobKeys) +
  the ready-edge seed hook (next to SendBlobDivergenceDeletes / the connect replay).
- `subsystems.cpp`: Install/Tick/OnDisconnect wiring + the connect-edge call.
- Docs: COOP_SYNC_MAP row, COOP_DISPATCH_VISIBILITY row, signals/TRACKER OPEN-9 -> BUILT,
  runbook layer.

## 10. /qf round map (10 race classes root-fixed)

R2 positional walk invalid -> set diff; R3 pointer identity dies at sort -> content-hash
multiset; R4 memo ABA killed + join gap permanent -> snapshot seed; R5 pending/seed
double-append -> pending exclusion; R6 pending-in-snapshot dup -> per-slot masks + client
pre-ready dup -> send gate; R10 armed-tombstone double delete -> mirrored mask (the R9
"idempotent via expiry" acceptance RETRACTED); R12 mask circularity -> op-counter
one-pass; R14 non-discriminating selftest -> injected 0->1->0. Zero probability-based
acceptances; every closure measured.

## 11. BUILD OUTCOME (2026-07-19, as-built deltas vs §1-§10)

1. **ORDER SUB-LANE ADDED (user decision mid-build: "Порядок да, надо синхронить per
   rule 1" — §7 item 1 superseded).** Order = STATE (mirror-state-not-verb):
   **MeadowOrder=114** blob {u16 n + n x u64 hashes in array order}, SAME lane as 112/113.
   HOST-CANONICAL (the RackState shape): a client move applies natively + sends its order
   to the host only; the host applies last-writer-wins + broadcasts ITS canonical; clients
   apply senderSlot==0 lines only; NOT relay-whitelisted. Detection: sortSignal = the 3rd
   0x45 matcher; baseline = the hash SEQUENCE (g_orderBase; appends push tail, deletes
   erase one instance, organic and wire alike); CommonOrderChanged = multiset-intersection
   filtered sequence compare (pure appends/deletes never trip it). Apply = validated
   byte-PERMUTE of the 0x70 rows (pointers move with their blocks; ReorderRows) +
   reflected genSignalList (the game's own widget re-applier). The join seed always ships
   one canonical order line after the deltas. **FIFO GUARD (order audit HIGH-1): every
   order send (poll / host rebroadcast / seed) is DEFERRED while any append/delete is
   pending — the lane pin orders only what was actually handed to GNS, so an order line
   referencing a pending hash would overtake it and strand the late append at the
   receiver's tail (permanent divergence). g_orderPending retries after the flush.**
2. **Perf audit C-1 (CRITICAL, fixed):** verb registration moved AFTER the throttled
   MS::EnsureResolved() gate; the lane reads MS::LaptopWidgetClass() (a getter) instead of
   its own FindClass — the unthrottled 60 Hz pre-world GUObjectArray walk is gone.
   W-1: the "reused scratch buffer" wording was wrong — HashStore allocates per row via
   Serialize (cold/edge only, never steady-state; accepted, Row hoisted). W-3 forever-
   latched engine ptrs = the fleet-standard v65 posture (inherited).
3. **Correctness audit (0 CRIT):** selftest remove is now RETRIED each poll to a 40 s
   deadline (a fire-and-forget failure could persist the synthetic row into the real
   save); the "no join snapshot" Warn is latched to the slot's FIRST replay
   (ConnectReplayForSlot re-fires per world-change re-announce — a consumed snapshot is
   normal there).
4. **Selftest row bug (smoke-diagnosed):** object/signal = the literal "None" string
   trips WriteFNameField's failed-intern check (StringToFName("None") == {0,0} ==
   NAME_None) — the synthetic row uses EMPTY strings; inject also retries with a
   discriminating step log (widget gate vs call refused). meadow_store::Widget() exits
   are now throttled-instrumented (dead-guard).
5. **Smoke evidence (final bytes 452973c707d9cb8d x4, proto 120):** PASS x2 (150 s);
   offsets resolved LIVE (laptop=0x448 saveSlot=0x4B0 savedSignals_0=0x680
   widget.laptop=0x920); prime + join snapshot + client apply chain proven: host inject ->
   client "applied append" -> digest 0->1 (sum 73680bb60361352e MATCHING) -> "applied
   delete" -> 0. The first proof run ALSO exercised pending+RetryPending live (the B2 gate
   skipped the not-yet-ready slot; the retry delivered both lines in FIFO order after
   world-ready). Zero meadow WARN/ERROR lines in either peer's log.
6. Files as built: meadow_db_sync.cpp 884 LOC + meadow_store.cpp 207 (under caps);
   net_pump.cpp 1229 / save_transfer.cpp 925 pre-existing over-cap (2-3-line additions
   here; extractions proposed: session/world_ready.cpp, save/join_baselines.cpp — queued
   alongside the drive_sync rack extraction).
