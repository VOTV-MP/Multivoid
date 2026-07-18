# Rack-lane extraction from drive_sync.cpp — mini owner-API design (s21b, 2026-07-18)

**Status: AS-BUILT (see §8). 8-round /qf "that holds" at R8/15; three commits landed
(0=promotion bc14fa33, A=instrument 5971cdd7, B=extraction); digest equality proven
cross-peer AND cross-commit; NOT hands-on.**
**Scope: PURE REFACTOR — no wire-format, no protocol, no behavior change; behavior preservation
is MEASURED (digest equality across commits), not asserted.**

Origin: drive_sync.cpp is 1007 LOC (past the 800 soft cap). The v119 design doc
(`votv-drive-chain-L5-impl-DESIGN-2026-07-18.md` :173-175) itself queued this extraction under
the name `drive_rack_sync.cpp` "before the next lane touches the file". The s21 first attempt
was deliberately deferred because a naive split would smear shared state; this design gives the
shared pieces an owner API first.

## §1 Measured coupling census (drive_sync.cpp read WHOLE at 035a6031)

Rack-ONLY (moves whole): g_rackDirty, g_rackBase, g_rackShadow, g_rackAsm, g_takenRing/next,
RackCanonicalBlob/ParseRackCanonical, HostBroadcastRackCanonical, ClientSendRackOp, SweepRacks
(the drain-before-adopt call from ClientApplyRackBlob :674 is rack-internal), HostApplyRackOp,
ClientApplyRackBlob, OnRackStateChunk, SnapshotRacks, the rack parts of QueueConnect/PrimeAll/
OnDisconnect, kind=2 pending entries, and the WHOLE deny axis: RecordDeny :219-226, TTL,
clear-on-consume :403-411, disconnect wipe :1000 (silent today).

Cross-lane (each measured):
- **g_denies**: armed by rack HostApplyRackOp op=1 deny (:589-604); consumed by the PAYLOAD
  lane's ApplyPayloadBlob host reap :401-412 with BOTH keys (`d.slot == senderSlot &&
  d.rowHash == rh` :404 — the v119 audit rejected slot-only; sender stayed as narrowing).
- **g_pending**: one vector, union kind 0=slot/1=payload/2=rack, one RetryPendingTick
  (:777-821). The v119 CRIT-1 stash-at-queue/drop-if-moved predicate is kind=0-ONLY (:787-797);
  rack kind=2 predicate = resolve-only (:810-816); replay fires only from the 1 Hz block (:895),
  no world-ready gate.
- **g_nextSeq**: shared by payload+rack blob sends; receiver assemblers are SEPARATE per kind
  (g_payloadAsm / g_rackAsm; Assembler keys (senderSlot, seq)) -> per-module counters are safe.
- **OnVerbEntry** (:231-269): ONE callback registered for 6 verb names. vm_dispatch MatchIndex
  (:121-127) is FIRST-match -> ONE cb per verb name fires; a second module registering
  "putDriveIn" would silently never fire (kMaxVerbs=16, 11 used). putDriveIn ctx-discriminates
  slot-role vs rack, and the rack case ALSO sets g_payloadDirty (:240, harvest zeroes a drive).
- **Refund path** (op=0 deny :560-576): spawns a drive picked up by SweepPayloads first-sight
  with authored=IsHost() (:339). Behavioral contract, ZERO calls into the payload lane (full
  callee enumeration done for every moving function; NoteLocalDriveBirth's sole caller is
  prop_drop_intent.cpp:311).
- **Ordering**: rack ops carry row CONTENT, not references (v119 doc :114) -> the harvest pair
  (rack canonical + zeroed drive payload) is two absolute idempotent states, order-agnostic
  between two modules' Ticks. Join seed: subsystems.cpp:252-282 is a straight source-order call
  chain; all three kinds pinned Lane::Normal (session_lanes.h:130-136) -> one per-connection
  FIFO = call order; shipped order (payloads -> racks) preserved by inserting the rack call
  right after drive_sync's (:267) and live-evidenced by v119/v120 smoke logs (:973 seed line).
  Reversal-benign is inferred-defensive only, NOT load-bearing.
- **Walks**: already TWO separate Registry::SnapshotActorsByType walks today (SnapshotDrives
  :153, SnapshotRacks :164); the split adds ZERO new walks.
- **Echo suppression**: SweepRacks has NO ScopedWireApply check — echoes are suppressed by
  priming base/shadow synchronously INSIDE applies (:685-691), and drain-before-adopt (:674)
  derives any in-flight local diff BEFORE a canonical lands -> no inter-tick window where wire
  content reads as organic.
- **ActorForEid census (corrected R5)**: THREE byte-identical copies exist today — drive_sync
  :141-148, floppybox_sync :124-131, laptop_sync's LidActorForEid :330-337 (renamed, NO class
  filter). trash_proxy's ProxyActorForEid is a DIFFERENT concept (proxy map) and stays.

## §2 The design

New module `coop/interactables/drive_rack_sync.{h,cpp}` (~450 LOC; drive_sync -> ~650):
own pending vector (resolve-only predicate, unchanged), own seq counter, own session atomic,
stats, 1 Hz cadence (sweep + assembler sweep + pending retry), prime-at-connect-edge,
OnDisconnect (INCLUDING the whole deny-ring wipe) + NEW teardown log lines in BOTH modules
(the wipe is silent today — guard-that-never-logs), QueueConnectBroadcastForSlot wired in
subsystems.cpp right after drive_sync's (join-seed byte order preserved). Standard module
shape = the v121 laptop_buffer_sync/floppybox_sync pattern.

Owner API — dependency strictly ONE-WAY drive_sync -> drive_rack_sync (callee-enum verified):
- `MarkDirtyFromVerb()` — atomic store; called from drive_sync's OnVerbEntry for
  putDriveIn-rack-ctx and getDrive. Capture-only, VM-bracket-safe (zero engine calls).
- `TryConsumeDenyReap(uint8_t senderSlot, uint64_t rowHash) -> bool` — does {match, TTL check,
  slot clear} inside rack (verdict axis = rack's, whole: ring+arm+TTL+clear+wipe); the reap
  ACTION (DestroyLocalProp + WARN + skip-apply) stays in ApplyPayloadBlob (the payload lane's
  response). Signature mirrors :404 1:1.

Verb registration stays SOLELY in drive_sync (forced by the measured one-cb-per-name
constraint; putDriveIn is shared slot/rack; the rack-ctx case keeps setting drive_sync's own
g_payloadDirty — harvest).

`ActorForEid` promoted to `coop::element::LivePropActor(ElementId)` (decl registry.h — 252
LOC, under cap; impl registry.cpp). All THREE identical copies deleted in the promotion
commit + negative grep.

Structural invariants (commit audit checklist for B):
- drive_rack_sync.{h,cpp} does NOT include drive_sync.h (anon-namespace :33 already makes
  static reach impossible — this pins the header edge too).
- Symbol-level negative grep: `g_denies|DenyRec|RecordDeny|g_takenRing` = 0 hits in
  drive_sync.cpp; `ActorForEid|LidActorForEid` local definitions = 0 hits in coop/ after 0.

Wire kinds/lanes/relay/protocol UNCHANGED (kProtocolVersion stays 121).

## §3 Considered + rejected
- Generic "value-ops box" template over rack/floppybox/meadow: deny/reap/refund semantics
  differ too much — over-abstraction now.
- Rack registering its own verbs: measured impossible for the shared putDriveIn (first-match)
  + verb-table pressure (11/16).
- No split: 1007 < 1500 is legal, but the soft-cap rule requires the proposal and the rack IS
  its own feature (floppybox_sync symmetry precedent).

## §4 Verification — the instrument and what it discriminates
Instrument = standalone `coop/dev/drive_selftest.cpp` ([dev] drive_selftest): uses ONLY public
APIs (DC::ReadRack/WriteRackRow/CallRackGen, signal_wire, Fnv64, Registry) -> the extraction
commit CANNOT touch the instrument. Digest = Fnv64 over serialized rack rows read directly via
DC::ReadRack — NO eid/seq/timestamps by construction; inject content = fixed constants.
Scenario per run: fresh join (connect seed + prime are INSIDE the digest surface — the client's
post-adopt digest asserts the seed path) -> host circle (organic WriteRackRow+CallRackGen ->
canonical -> client adopt, cross-peer digest match) -> client circle (local write outside any
guard -> SweepRacks derives op=0 -> host apply -> canonical back, digest match; op-count==1
asserted via the verbatim-preserved log strings).
The client circle exercises the SAME SweepRacks body as a player action (no guard check in
SweepRacks; the 1 Hz trigger is itself a production path — the matcher-gap fallback).

## §5 The three commits
0. **LivePropActor promotion** — three identical bodies -> one registry fn; same-commit
   deletion + negative grep; standard smoke exercises all three 1 Hz call paths, zero new WARN.
1. **(A) drive_selftest + BASELINE x2** on the unsplit (but promoted) code — run the scenario
   TWICE to prove run-to-run digest stability before anything moves.
2. **(B) The extraction** — same scenario re-run -> digests byte-equal cross-peer AND
   cross-commit; PLUS a MANDATORY reconnect cycle: client exit (teardown log observed = the
   wipe measured) -> client rejoin -> re-seed digest equals again (closes the OnDisconnect
   residual and repeat-seed cleanliness in one run).

Single remaining honest residual: the deny/reap two-peer same-frame race is not autonomously
exercisable -> mechanical-move diff review + the take-4 hands-on runbook step.
Status after all three commits = **AS-BUILT** (not VERIFIED; hands-on pending).

## §6 /qf round map (8 rounds, fresh critic each)
R1 one-way dep -> callee enum; ordering flag lifted (v119 doc :114); walks 2->2; join order
   measured; name adopted from v119 doc :173-175.
R2 senderSlot real in the reap (:404); RULE-2 same-commit copy deletion; interaction-smoke plan.
R3 CRIT-1 predicate kind=0-only stays; rack replay edge unchanged; structural invariants;
   two-commit digest plan; utility home = registry.h.
R4 digest = final canonical state (discriminates the axis); WHOLE deny ownership moves;
   client circle passes the organic detect (measured).
R5 instrument isolated in a dev TU; census CORRECTED to three copies; drain-before-adopt
   closes the inter-tick window (measured).
R6 promotion split into its own commit 0; connect seed inside the digest surface; OnDisconnect
   residual flagged; shipped-order (measured) vs reversal-benign (inferred-defensive) split.
R7 baseline x2 (stability); teardown logs added (the silent wipe becomes measured); reconnect
   cycle promoted to mandatory.
R8 "that holds".

## §8 BUILD OUTCOME (2026-07-18 evening — AS-BUILT, not hands-on)

Commits: **0** = `bc14fa33` (LivePropActor promotion; negative grep clean; note: the census
sharpened during rounds — laptop's LidActorForEid was the 3rd byte-identical copy, so
rule-of-three was met even before the extraction). **A** = `5971cdd7` (drive_selftest; its
subsystems/CMake wiring rides commit B — the files interleave with the extraction wiring;
the instrument FILES are identical across A/B, which is what comparability needs). **B** =
the extraction (this commit). DLL `6431c14382b38437` x4 hash-verified, proto 121 unchanged.

Instrument note (run-1 lesson): a fresh save has NO rack (shop item) — the host seeds one
via SpawnActor at the desk slot ~5 s after connect; the spawn fans to the client via the
host watcher. Digest values are eid-independent (proven: eids 6102/6147/6103 across runs,
same three digests).

**Digest equality (the §4 measurement):** baseline x2 on the UNSPLIT DLL and the extraction
run all produced the SAME three values — empty `cea1940d5997c1f3`, host row
`c4b0a7012829f902`, client row `2f57cabc0b11d213` — cross-peer (empty + client-circle
states on both peers; the host-filled state's client tick-window artifact reproduces
IDENTICALLY in baseline and B, preserving comparability) and cross-commit. Op sequences
verbatim modulo the intended drive_sync->drive_rack log-prefix rename. Smokes: baseline x2
PASS + commit-B PASS (mp.py smoke, RSS stable ~3.2 GB, zero module WARN/failures).

**Reconnect cycle (mandatory per R7):** host stayed up; client joined -> circles -> client
killed -> relaunched -> rejoined. Host log shows the rising-edge re-prime x2 ("drive_rack:
baselines primed (connect seed)" 18:42:57 + 18:44:51) + TWO full connect seeds ("1 racks"
each) + the phase-2 client's adopt and a full second circle (host applied set/take
18:45:36/44). Teardown logs added to both modules' OnDisconnect (was silent).

**Audits:** perf — PASS, 0 CRITICAL (table in the audit transcript; walk count 2->2,
MarkDirtyFromVerb atomics-only, all Installs O(1)-latched under the 125 Hz contract; one
dev-only WARN: drive_selftest's ResolveRack walks at 125 Hz while no rack resolves —
ini-gated default-0, throttle queued if the probe ever gets long exposure). correctness —
FAITHFUL per function; both flagged doc-drifts fixed (drive_sync.h teardown comment;
COOP_SYNC_MAP row). Belt-and-suspenders literal git-diff of the moved bodies vs
5971cdd7~2: all SAME after the two intended renames; sole difference = one comment word in
HostApplyRackOp.

**Residual DISCOVERED (pre-existing, NOT this refactor's):** a rejoining client whose
downloaded save CONTAINS the rack ends up with ONE actor under TWO identities — the
save-loaded prop takes a provisional client-band eid (45223) BEFORE CreateOrAdoptPropMirror
binds the host eid (6103) to the same key; both identities stay live, so client-side organic
changes emit a doomed op under the client-band eid (silently dropped by the host — 
LivePropActor fails) alongside the good op under the host eid; the canonical heals
convergence. This is the join-window save-loaded-keyed-prop identity class
(docs/COOP_STABLE_ID_SIDECAR.md thread), first EXPOSED here because no prior test ever
joined a world whose save contained a rack. Evidence: scratchpad
rackB_reconnect_CLIENT_phase2.log (18:45:10-44). Queued for the stable-ID thread, not a
lane bug (the pre-split code behaves identically).
**RESOLVED 2026-07-18 s22 (`4403606c` v122 no-passive-mint):** the residual was the tip of a
~2200-per-join zombie class (client passive census minting silent keyed Elements + the adopt
stacking a mirror + the host reverse-steal door). Root fix B/S/A'/H; the SAME reconnect
scenario re-run measures the rack under ONE host-band eid with single ops and matching digest
circles. See votv-stable-id-no-passive-mint-DESIGN-2026-07-18.md (AS-BUILT).

Honest status: **AS-BUILT** — smoke + digest-equality + reconnect evidence, no hands-on.
