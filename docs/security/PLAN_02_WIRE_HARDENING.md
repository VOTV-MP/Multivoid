# Plan 02 — wire hardening (apply-side caps and validation)

**Closes:** `TRACKER` **W1, W2, W3, W4, W5, W6, W7, W8, W9, W10**.
**Status: DESIGN. No code written.** W1/W2/W3/W6 are `[V]` personally verified; W4/W5/W7-W10 remain
`[A]` and must be re-read before their fix (Rule S5).

---

## 1. The one root

**Rule S3: caps belong on the apply side, not only the send side.** Ten findings, one mistake
repeated at ten seams — every one assumes the peer runs our sender.

The diagnostic that finds the rest of them: *"if the peer sends the largest value this field can
hold, what does the receiver allocate?"*

**This plan is therefore also a pattern rollout, not just ten patches.** Fixing the ten without
establishing the pattern guarantees an eleventh.

---

## 2. Wave 1 — remote process kill (CRITICAL) — **BUILT `6f0c2bf8`**

**Only W1 was ever a one-packet kill.** `[V]` W2 needs a complete CRC-valid transfer
(`DeserializeSidecar` runs at `save_transfer.cpp:313`, after the CRC gate at `:292-297`) and W3 is a
grind. The earlier text here said "all three kill with one packet" — that was wrong.

### W1 — `OnBegin` reserved from an uncapped `uint32_t` `[V]` — **BUILT**

- **Site:** `save_transfer.cpp:857` — `g_cliBuf.reserve(p.totalBytes)`. The only guard above it was
  `p.totalBytes == 0` (`:845`).
- **Attack:** hostile host sends `totalBytes = 0xFFFFFFFF` → 4 GiB reserve → uncaught `bad_alloc`.
  No try/catch around the reliable drain, so the process **terminates**.
- **The fix is a DELETION, not a cap** — and this reframe is the most useful thing in the plan. A
  `/qf` round asked whether the reserve was a pure allocation hint. `[V]` A full use census of
  `g_cliTotal` says yes: its only other uses are the completion compare (`:284`), the overflow bound
  (`:285`) and the progress readback (`:871`). Removing the reserve removes the allocation entirely
  and costs ~9 vector doublings across a multi-second download.
- **Why capping would have been worse:** a cap is a *policy limit* that can reject a legitimately
  grown world and drop that player into the fresh-world fallback. The bug it prevents is a crash;
  the bug it introduces is a silent degradation for honest users. Deleting the primitive has neither.
- **Evidence:** smoke transferred **20 990 211 B / 367 chunks**, CRC ok, slot written, after removal.
- **NOT verified:** no crafted `Begin` was ever sent. The rejection path is untested (see §7).

### W1b — no guard against a SECOND `Begin` `[V]` — **BUILT** (new; neither audit found it)

- **Site:** `save_transfer.cpp:842-859`. `g_cliTotal`, `g_cliChunkCount`, `g_cliCrc` and
  `g_cliSidecarBytes` were reassigned mid-`Receiving` by any later host packet — moving the
  completion denominator and the CRC out from under a stream already in flight.
- **Why it is always a violation:** `[V]` `g_cliHaveBegin` is cleared **only** by `ClientArm`
  (`:821`) and the teardown (`:901`), so within one arm a second `Begin` can never be legitimate.
  Fail loudly rather than ignore — ignoring would leave the denominator describing a stream that no
  longer exists.
- **How it was found:** by censusing the **writers** of `g_cli*` after twice censusing only the
  **uses**. That "uses, not writers" habit is the actual defect; see §5.

### W2 — `DeserializeSidecar` reserved from an unvalidated count `[V]` — **BUILT**

- **Site:** `save_identity_map.cpp:266` — `outMap.reserve(count)` where `count` is read straight from
  the wire at `:262`.
- **Note the shape:** the walk that follows (`:269+`) is **correctly bounds-checked** and aborts on
  truncation. The reserve happens *before* it. This is a good example of a careful parser undone by
  one line above it.
- **Runs on the net thread.**
- **Fix:** before reserving, require `count <= (len - kSidecarHeaderBytes) / kSidecarFixedEntryBytes`
  — entries are variable-length with a trailing key, so this is a **ceiling**, and any `count` above
  it cannot possibly be satisfiable. Cheap, exact, no new constant.
- **Acceptance:** a sidecar with `count = 0xFFFFFFFF` and a 40-byte body returns `false` without
  allocating.

### W3 — chunks accepted before `Begin`, with no cap `[V]` — **STILL OPEN, deliberately**

- **Site:** `save_transfer.cpp:363-379` (`BulkSink_`). In `WaitingBegin` the sink transitions to
  `Receiving` and appends; `MaybeFinishLocked_` returns immediately at `:283` because
  `!g_cliHaveBegin`, so the `size() > g_cliTotal` overflow check at `:285` **never runs**.
- **Post-Begin is already bounded** `[V]`: with `haveBegin` set, every append calls
  `MaybeFinishLocked_` (`:378`), which fails the transfer the moment `size > g_cliTotal`. So the
  **only** unbounded path is bytes with no announced denominator.
- **The pre-Begin window is LEGITIMATE, and this killed the obvious fix** `[V]`: it is not wire
  reordering — chunks and `Begin` ride the *same* in-order reliable lane (`:543` vs `:200`). It is
  **cross-thread lag**: `OnBegin` runs on the game thread (`save_transfer.h:151-152`) while
  `BulkSink_` runs on the net thread (`:361`), both under `g_cliMu`. "Refuse chunks before Begin"
  would break real joins.
- **Why nothing was shipped:** the remaining options were a *sized* pre-Begin window — a number I
  cannot measure and would be keeping only so the finding has a fix attached — or the root fix.
  Shipping the number would be a crutch (RULE 1).
- **The root fix (next commit): latch `Begin`'s four scalars on the NET thread under `g_cliMu` at
  arrival**, leaving the game thread its user-visible work. That makes `WaitingBegin`-with-bytes
  *structurally impossible* and deletes the window instead of sizing it — no constant at all.
  `[V]` The seam exists: `session.cpp:414-416` already diverts `SaveTransferChunk` to a net-thread
  sink; `SaveTransferBegin` currently goes to the inbox → `event_feed.cpp:164` → game thread.
- **Why it is its own commit:** it touches the session router on the **join critical path**, which
  is hands-on-tested territory. Bundling a router change into "three tiny checks" is how a join
  path breaks. W3 is a grind, not a one-packet kill, so the sequencing cost is low.
- **Also owed with it** `[V]`: `BulkSink_:370` never checks `len` against `kSaveChunkBytes`
  (`protocol.h:3253` = 56 KB) while the receive side allows up to a u16 `payloadLen`. That check is
  exact and constant-free — the constant already exists.

---

## 3. Wave 2 — amplification and cross-peer corruption (HIGH)

### W4 — `blob_chunks::Assembler` amplification `[A]`

- **Claimed:** `blobSeq` is attacker-chosen and default-inserts a map node, then reserves
  `chunks * 220` — one 228-byte packet costs ~56 KB (**~246x**). Reachable client→host with **no join
  or role gate** on 8 lanes. `laptop_sync`'s sweep sits behind `EnsureResolved()`, so a peer stuck in
  a menu never reclaims.
- **Precedent in-tree:** `order_sync.cpp:268` already caps its table — which is what makes this an
  oversight rather than a design position.
- **Fix:** per-sender assembly cap in `blob_chunks` itself, mirroring `order_sync`'s `kMaxAssembly`.
  Fixing it at the shared primitive covers all 8 lanes at once — **do not patch the lanes.**
- **Verify first:** the amplification ratio and the 8 lane list.

### W5 — `owner_entity_sync` spawns 65 536 actors per peer `[A]`

- **Claimed:** `kMaxOwned = 8` is **send-side only** (`owner_entity_sync.cpp:350`); the receive path
  spawns for every unseen `(slot, seq)`. On the relay whitelist, so it reaches every client.
- **Fix:** receive-side `g_mirrors` cap per sender, at the same value as the send-side intent.
- **Note:** this is Rule S3 in its purest form — the constant already exists and expresses the right
  intent; it is simply enforced on the wrong side.

### W6 — `TrashCarryPose`: no role gate, no finite check `[V]`

- **Site:** `session_trashcarry.cpp:58-70`. `[V]` the file contains **zero** occurrences of
  `IsHost`, `senderPeerSlot`, `isfinite`, `ValidatePose`, or `FiniteVec`.
- **The asymmetry is the proof:** `[V]` its five siblings in `session_streams.cpp` all validate —
  `:198` `ValidatePose`, `:222`, `:260`, `:297`, `:324` `isfinite`. This one lane was missed.
- **The false comment:** `:61-62` claims "Per-entry float validation + the ctx-freshness gate happen
  at the game-thread apply". `[V]` **Half true.** The ctx gate is real
  (`trash_clump_pose_stream.cpp:49`); the float validation exists nowhere on the path — the values go
  from `:60` straight into `BeginLerpToPose`. NaN reaches `SetActorLocation`.
- **Fix:** role gate (host-originated only, matching the comment's own claim at `:58-60`) + finite
  check, matching `npc_mirror.cpp:638`. **And correct the comment** (Rule S1).
- **Acceptance:** a client-sent `TrashCarryPose` is dropped by role; a NaN-bearing host batch is
  dropped by the finite check.

---

## 4. Wave 3 — bounded growth and fairness (MEDIUM)

| ID | Fix | Note |
|---|---|---|
| **W7** | Allow-list / charset the wire strings that reach `StringToFName`, or intern against a bounded set | UE never frees FName entries, so this is permanent process-lifetime growth. The charset is the cheaper half |
| **W8** | Enforce the existing caps at apply — the floppybox 15-entry cap is a `UE_LOGW` that **does not reject** | A cap that logs and proceeds is not a cap; same family as Rule S3 |
| **W9** | Bound `g_lidPending` and `g_tombs` | `g_lidPending[p.eid]` inserts precisely when the eid does **not** resolve — the garbage case is the inserting case |
| **W10** | Per-peer accounting on the reliable inbox (`kReliableInboxCap = 8192` is global) | One flooding peer currently starves every other peer's events |

---

## 5. The pattern rollout (so there is no eleventh)

After Wave 1, before Wave 3:

0. **Adopt MTA's `CanReadNumberOfBytes` as the invariant form.** `[A]` `MTA_PRECEDENT.md` §4:
   *a length read off the wire is validated against the bytes actually remaining in the datagram
   before it sizes an allocation* (`bitstream.h:219`, used at `:292` before the `resize` at `:296`),
   generalized to counts as *count x min-encoded-size <= remaining* (`CLuaArguments.cpp:543-549`).
   **Our W1/W2 fixes are correct but site-by-site; this is the shape that removes the class.** W4 and
   W5 should be built in this form rather than as two more bespoke caps. MTA also separates parse from
   apply entirely (`CPacketTranslator.cpp:250-256`) — a failed `Read()` destroys the packet before any
   handler sees it, which is why their handlers do not carry validation the way ours do.
0b. **Census WRITERS, not just USES.** W2 and W1b are the same defect twice: a field's *readers* were
   enumerated and its *writers* were not, so nobody asked "who else can move this, and when?"
   `[V]` A writer census of `g_cli*` found W1b in minutes after two rounds of use-censuses missed
   it. For any wire-fed state field, list every assignment site and every thread that reaches it.
1. **Write the receive-side checklist** into `docs/COOP_SYNC_MAP.md` where new lanes are designed:
   *every `reserve`/`resize` from a wire value needs a cap; every float needs `isfinite`; every lane
   needs an explicit role gate; every per-sender map needs a bound; every announce-then-stream lane
   needs a "announced once" invariant.*
2. **Audit the existing lanes against it once**, as a sweep rather than per-finding — the ten rows
   here came from two agents looking at part of the surface.
3. **Add it to the audit prompt template** (`reference/agency-agents/audit-prompt-perf-template.md`
   already carries the file-size check; this is the security twin).

---

## 6. Sequencing and risk

| Wave | Items | Risk | Gate |
|---|---|---|---|
| 1 | W1, W2, W3 | **Low** — pure rejection paths on failure branches | None. Do it next |
| 2 | W4, W5, W6 | **Medium** — W4 touches a shared primitive used by 8 lanes; W6 changes an active stream | Verify W4/W5 sites first (Rule S5); smoke both peers |
| 3 | W7-W10 | Low-medium — W7 could reject legitimate names if the charset is too tight | Needs a real-name census before choosing the charset |

**A protocol version bump is not required** for Wave 1-3: these are rejection paths, not format
changes. Confirm per-edit — if any fix changes a field's meaning, the standing rule applies.

**Smoke requirement:** W6 and W4 touch live streams. Per the pre-deploy checklist, both need a 30 s
two-peer smoke with clean logs before handoff, not just a build.

---

## 7. The hostile-host drill (owed — this is what BUILT is missing to become VERIFIED)

Wave 1 shipped **rejection paths that have never rejected anything.** The smoke proves the honest
path still works; it proves nothing about the guard. Per `EXECUTION.md` §2 requirement 3, an
attempted exploit must be shown failing.

**The technique already exists in this project:** the version-gate work drilled its refusal path
with a deliberately-wrong build (the "NOVAL" build). The same shape applies here — a host build that
sends a crafted `Begin`.

| Drill | Crafted value | Expected |
|---|---|---|
| **W1** | `totalBytes = 0xFFFFFFFF` | Client alive; transfer `Failed`; one `[Error]`; falls back to fresh world and still joins |
| **W1b** | A valid `Begin`, then a second `Begin` mid-stream | `"second Begin during an active transfer"` logged; `Failed` |
| **W2** | `sidecarBytes` framing with `count = 0xFFFFFFFF` in a small body | `DeserializeSidecar` returns false without allocating; the "identity sidecar parse failed" branch runs; the `.sav` still writes |

Until this runs, W1/W1b/W2 stay **BUILT**, never **VERIFIED**.

---

Back to: `README.md` · `TRACKER.md` W-rows · `RULES.md` S3.
