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

## 2. Wave 1 — remote process kill (CRITICAL, cheap, do first)

Each is a small apply-side cap and each currently lets **one packet kill a joining client**.

### W1 — `OnBegin` reserves from an uncapped `uint32_t` `[V]`

- **Site:** `save_transfer.cpp:857` — `g_cliBuf.reserve(p.totalBytes)`. The only guard above it is
  `p.totalBytes == 0` (`:845`).
- **Attack:** hostile host sends `totalBytes = 0xFFFFFFFF` → 4 GiB reserve → uncaught `bad_alloc`.
  There is no try/catch around the reliable drain, so the process **terminates**.
- **Correction to the earlier tracker text:** it proposed capping against `kMaxSaveBlobBytes`.
  `[V]` **that constant does not exist anywhere in the tree.** The fix must define it.
- **Fix:** define `kMaxSaveBlobBytes` in `protocol.h` next to the other wire caps. Real saves are
  ~17 MB; set the cap with headroom (proposed **64 MB**) and reject `Begin` above it with a logged,
  user-visible failure — not a silent drop, since a legitimate oversized save must be diagnosable.
- **Acceptance:** a crafted `Begin` with `totalBytes = 0xFFFFFFFF` leaves the client alive, logs one
  `[Error]`, and puts the transfer in `Failed`.

### W2 — `DeserializeSidecar` reserves from an unvalidated count `[V]`

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

### W3 — chunks accepted before `Begin`, with no cap `[V]`

- **Site:** `save_transfer.cpp:363-379` (`BulkSink_`). In `WaitingBegin` the sink transitions to
  `Receiving` and appends; `MaybeFinishLocked_` returns immediately at `:283` because
  `!g_cliHaveBegin`, so the `size() > g_cliTotal` overflow check at `:285` **never runs**.
- **Attack:** a host streams chunks and never sends `Begin` → `g_cliBuf` grows without limit.
- **Refinement over the `[A]` text:** there **is** a sequential index check at `:367-372`, so the
  attacker must send chunks in order — trivially satisfiable, so severity is unchanged, but the
  exploit is not "spray arbitrary chunks".
- **Fix:** cap `g_cliBuf` in the sink itself against `kMaxSaveBlobBytes` (the W1 constant),
  regardless of state. One shared cap for one buffer.
- **Acceptance:** a peer streaming endless in-order chunks with no `Begin` is cut off at the cap and
  the transfer fails cleanly.

**Wave 1 is three edits, two files, one new constant.** It should be one commit.

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

1. **Write the receive-side checklist** into `docs/COOP_SYNC_MAP.md` where new lanes are designed:
   *every `reserve`/`resize` from a wire value needs a cap; every float needs `isfinite`; every lane
   needs an explicit role gate; every per-sender map needs a bound.*
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

Back to: `README.md` · `TRACKER.md` W-rows · `RULES.md` S3.
